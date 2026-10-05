#include "omnimesh/execution.hpp"
#include "omnimesh/control_plane.hpp"
#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>

namespace omnimesh {
namespace {
// Local sessions share no durable allocator. Serialize them for this host UID
// instead of allowing two private allocators to overcommit the same node.
class LocalAuthority {
public:
  ~LocalAuthority() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  Status acquire() {
    const auto path =
        "/tmp/omnimesh-local-agent-" + std::to_string(getuid()) + ".lock";
    fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat info{};
    if (fd_ < 0 || fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != getuid() || (info.st_mode & 0777) != 0600 ||
        info.st_nlink != 1) {
      return {StatusCode::permission_denied,
              "cannot acquire a private local-agent authority file"};
    }
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      return {StatusCode::conflict,
              "another local-agent session holds authority for this host user"};
    }
    if (fstat(fd_, &info) != 0 || info.st_size != 0) {
      return {StatusCode::conflict,
              "previous local-agent execution is uncertain; inspect the "
              "session named in the authority file before manual recovery"};
    }
    return Status::Ok();
  }
  Status mark(const std::string &session) {
    marked_ = true;
    const auto bytes = session + "\n";
    if (pwrite(fd_, bytes.data(), bytes.size(), 0) !=
            static_cast<ssize_t>(bytes.size()) ||
        ftruncate(fd_, static_cast<off_t>(bytes.size())) != 0 ||
        fsync(fd_) != 0) {
      return {StatusCode::internal,
              "cannot persist local execution recovery barrier"};
    }
    return Status::Ok();
  }
  void clear() {
    if (marked_ && ftruncate(fd_, 0) == 0) {
      fsync(fd_);
      marked_ = false;
    }
  }

private:
  int fd_{-1};
  bool marked_{false};
};
bool terminal(TaskState state) {
  return state == TaskState::succeeded || state == TaskState::failed ||
         state == TaskState::cancelled;
}
std::string host_architecture() {
  utsname info{};
  if (uname(&info) != 0) {
    return {};
  }
  const std::string machine = info.machine;
  return machine == "x86_64" ? "amd64" : machine == "aarch64" ? "arm64" : "";
}
} // namespace
LocalExecutionResult execute_local(const Workload &workload, const Node &node,
                                   const LocalExecutionOptions &options,
                                   const std::function<bool()> &cancelled) {
  LocalExecutionResult result;
  const auto deadline =
      MonotonicClock::now() + std::chrono::milliseconds(options.timeout_millis);
  Allocator allocator;
  WorkloadController controller(allocator);
  LocalAuthority authority;
  DurableControlPlane durable;
  const bool journal = !options.journal_directory.empty();
  std::set<std::string> journaled_attempts;
  bool cancel_recorded = false;
  const auto workload_id = workload.tenant + "/" + workload.name;
  auto finish = [&](Status status) {
    result.status = std::move(status);
    if (journal) {
      result.journaled = true;
      result.journal_records = durable.stats().records;
      const auto closed = durable.close();
      if (!closed.ok() && result.status.ok()) {
        result.status = closed;
      }
    }
    controller.inspect(workload_id, workload.tenant, result.workload);
    const auto snapshot = allocator.snapshot();
    const auto tenant = snapshot.tenants.find(workload.tenant);
    result.reservations_retained = tenant != snapshot.tenants.end() &&
                                   tenant->second.active_allocations != 0;
    if (!result.reservations_retained) {
      authority.clear();
    }
    return result;
  };
  // Records cancellation before the controller mutation it describes. A
  // recording failure ends the session fail-closed without cancelling.
  auto note_cancellation = [&]() -> Status {
    if (!journal || cancel_recorded) {
      return Status::Ok();
    }
    cancel_recorded = true;
    return durable.record_cancellation(workload_id);
  };
  if (options.timeout_millis == 0 || options.timeout_millis > 3600000 ||
      options.grace_millis > 60000 ||
      workload.replicas *
              static_cast<std::uint64_t>(workload.retry.max_attempts) >
          64 ||
      !workload.capabilities.empty()) {
    return finish(
        {StatusCode::invalid_argument,
         "local session requires a 1-3600000 ms timeout, 0-60000 ms grace, at "
         "most 64 total attempts and no optional capabilities"});
  }
  if (node.platform.architecture != host_architecture()) {
    return finish({StatusCode::unavailable,
                   "node architecture does not match this host"});
  }
  const bool from_image = !options.image_layout.empty();
  if (options.rootfs.empty() == options.image_layout.empty()) {
    return finish({StatusCode::invalid_argument,
                   "select exactly one of rootfs or image layout"});
  }
  if (from_image &&
      (options.max_image_bytes < 1024ULL * 1024ULL ||
       options.max_image_bytes > 16ULL * 1024ULL * 1024ULL * 1024ULL)) {
    return finish(
        {StatusCode::invalid_argument, "image byte bound is out of range"});
  }
  auto status = Status::Ok();
  if (journal) {
    // The journal directory must not be the session directory: opening it
    // creates it, and the session directory is required to be fresh.
    std::error_code path_error;
    const auto journal_path =
        std::filesystem::weakly_canonical(options.journal_directory, path_error);
    const bool journal_path_ok = !path_error;
    const auto state_guess = std::filesystem::weakly_canonical(
        options.state_directory, path_error);
    if (!journal_path_ok || path_error || journal_path == state_guess) {
      return finish({StatusCode::invalid_argument,
                     "journal directory must be usable and differ from the "
                     "session state directory"});
    }
    status = durable.open(options.journal_directory);
    if (!status.ok()) {
      return finish(status);
    }
    // Facts are recorded before the mutations they describe; recovery replays
    // them in order and restores active attempts as Unknown.
    status = durable.record_node(node);
    if (!status.ok()) {
      return finish(status);
    }
  }
  status = allocator.upsert_node(node);
  if (!status.ok()) {
    return finish(status);
  }
  const TenantQuota session_quota{node.capacity, 1};
  if (journal) {
    status = durable.record_quota(workload.tenant, session_quota);
    if (!status.ok()) {
      return finish(status);
    }
    status = durable.record_workload(workload);
    if (!status.ok()) {
      return finish(status);
    }
  }
  status = allocator.set_quota(workload.tenant, session_quota);
  if (!status.ok()) {
    return finish(status);
  }
  status = controller.submit(workload, workload.tenant);
  if (!status.ok()) {
    return finish(status);
  }
  status = authority.acquire();
  if (!status.ok()) {
    return finish(status);
  }
  if (options.runtime_executable.empty() ||
      options.runtime_executable.front() != '/' ||
      options.runtime_executable.find('\0') != std::string::npos ||
      options.runtime_executable.size() > 4096 ||
      access(options.runtime_executable.c_str(), X_OK) != 0) {
    return finish({StatusCode::unavailable,
                   "select an executable OCI runtime using an absolute path"});
  }
  std::error_code error;
  const auto &source = from_image ? options.image_layout : options.rootfs;
  if (source.empty() || source.front() != '/' ||
      source.find('\0') != std::string::npos || source.size() > 4096) {
    return finish({StatusCode::invalid_argument,
                   "rootfs or image layout must be an absolute path"});
  }
  const auto source_path = std::filesystem::canonical(source, error);
  if (error || source_path == "/" ||
      !std::filesystem::is_directory(source_path, error) || error) {
    return finish(
        {StatusCode::invalid_argument,
         "rootfs or image layout must be an existing directory other than /"});
  }
  if (options.state_directory.empty() ||
      options.state_directory.front() != '/' ||
      options.state_directory.find('\0') != std::string::npos ||
      options.state_directory.size() > 4096) {
    return finish({StatusCode::invalid_argument,
                   "state directory must be an absolute path"});
  }
  const auto state_path =
      std::filesystem::weakly_canonical(options.state_directory, error);
  if (error || state_path == source_path ||
       state_path.string().compare(0, source_path.string().size() + 1,
                                   source_path.string() + "/") == 0) {
    return finish(
        {StatusCode::invalid_argument,
         "state directory must be outside the rootfs or image layout"});
  }
  if (from_image) {
    const auto placement =
        propose_placement(requirements_for(workload), allocator.snapshot());
    if (!placement.status.ok()) {
      return finish(placement.status);
    }
  }
  // An exclusive private session directory is also a restart barrier. Existing
  // state is never overwritten or guessed to be safe after a crash.
  if (mkdir(state_path.c_str(), 0700) != 0) {
    return finish(
        {StatusCode::conflict, "state directory must be fresh; inspect "
                               "existing runtime state before recovery"});
  }
  result.session_directory = state_path.string();
  auto rootfs = source_path;
  if (from_image) {
    rootfs = state_path / "rootfs";
    UnpackOptions unpack;
    unpack.layout_directory = source_path.string();
    unpack.rootfs_directory = rootfs.string();
    unpack.platform = {node.platform.os, node.platform.architecture};
    const auto at = workload.image.rfind('@');
    unpack.expected_digest = at == std::string::npos
                                 ? workload.image
                                 : workload.image.substr(at + 1);
    unpack.max_bytes = options.max_image_bytes;
    bool preparation_cancelled = false;
    unpack.cancelled = [&] {
      preparation_cancelled = preparation_cancelled ||
                              MonotonicClock::now() >= deadline ||
                              (cancelled && cancelled());
      return preparation_cancelled;
    };
    result.rootfs_directory = rootfs.string();
    ImageLoader loader;
    status = loader.unpack(unpack, result.image);
    if (!status.ok()) {
      const auto failure = status;
      status = note_cancellation();
      if (!status.ok()) {
        return finish(status);
      }
      controller.cancel(workload_id, workload.tenant);
      return finish(failure);
    }
    result.image_verified = true;
  }
  result.rootfs_directory = rootfs.string();
  const auto runtime_root = result.session_directory + "/runtime";
  if (mkdir(runtime_root.c_str(), 0700) != 0) {
    return finish(
        {StatusCode::internal, "cannot create private runtime state"});
  }
  OciRuntime runtime(options.runtime_executable, runtime_root);
  bool session_cancelled = false;
  bool cleanup_failed = false;
  std::size_t ordinal = 0;
  while (true) {
    if (MonotonicClock::now() >= deadline || (cancelled && cancelled())) {
      session_cancelled = true;
      status = note_cancellation();
      if (!status.ok()) {
        return finish(status);
      }
      status = controller.cancel(workload_id, workload.tenant);
      if (!status.ok()) {
        return finish(status);
      }
    }
    status = controller.reconcile();
    if (!status.ok()) {
      return finish(status);
    }
    WorkloadRecord record;
    status = controller.inspect(workload_id, workload.tenant, record);
    if (!status.ok()) {
      return finish(status);
    }
    if (journal) {
      // Reservations are committed inside reconcile; they are recorded here,
      // before any start intent or runtime operation for the new attempt. A
      // crash between commit and this record loses the fact, so recovery can
      // only restore what was recorded, never assume completeness.
      for (const auto &item : record.tasks) {
        if (item.state != TaskState::allocated || item.attempts.empty()) {
          continue;
        }
        const auto &latest = item.attempts.back();
        if (!journaled_attempts.insert(latest.id).second) {
          continue;
        }
        status = durable.record_reservation(item.id, latest.node_id,
                                            latest.number, latest.generation);
        if (!status.ok()) {
          note_cancellation();
          controller.cancel(workload_id, workload.tenant);
          return finish(status);
        }
      }
    }
    if (std::all_of(
            record.tasks.begin(), record.tasks.end(),
            [](const TaskRecord &task) { return terminal(task.state); })) {
      const bool succeeded = std::all_of(
          record.tasks.begin(), record.tasks.end(), [](const TaskRecord &task) {
            return task.state == TaskState::succeeded;
          });
      return finish(
          succeeded && !cleanup_failed
              ? Status::Ok()
              : Status{
                    StatusCode::unavailable,
                    cleanup_failed ? "work stopped but runtime cleanup failed; "
                                     "inspect the session directory"
                    : session_cancelled
                        ? "local execution cancelled or exceeded its deadline"
                        : "one or more tasks failed"});
    }
    auto task = std::find_if(record.tasks.begin(), record.tasks.end(),
                             [](const TaskRecord &item) {
                               return item.state == TaskState::allocated;
                             });
    if (task == record.tasks.end()) {
      // Retry backoff uses the same steady clock as the controller. A placement
      // rejection is returned immediately rather than spinning until timeout.
      bool waiting_retry = false;
      for (const auto &item : record.tasks) {
        waiting_retry = waiting_retry || (item.state == TaskState::queued &&
                                          !item.attempts.empty());
      }
      if (!waiting_retry) {
        return finish({StatusCode::unavailable,
                       "local workload cannot be placed on the supplied node"});
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    const auto attempt = task->attempts.back();
    WorkerResult worker;
    worker.attempt_id = attempt.id;
    worker.container_id = "omni-" + std::to_string(++ordinal);
    Allocation allocation;
    status =
        allocator.inspect(attempt.allocation_id, workload.tenant, allocation);
    if (!status.ok()) {
      return finish(status);
    }
    const auto bundle =
        result.session_directory + "/bundle-" + std::to_string(ordinal);
    status = prepare_bundle(workload, allocation, rootfs.string(), bundle);
    if (!status.ok()) {
      const auto failure = status;
      status = note_cancellation();
      if (!status.ok()) {
        return finish(status);
      }
      controller.cancel(workload_id,
                        workload.tenant); // No start intent was issued.
      return finish(failure);
    }
    status = authority.mark(result.session_directory);
    if (!status.ok()) {
      const auto failure = status;
      status = note_cancellation();
      if (!status.ok()) {
        return finish(status);
      }
      controller.cancel(workload_id, workload.tenant);
      return finish(failure);
    }
    status = controller.begin_start(attempt.id, workload.tenant);
    if (!status.ok()) {
      return finish(status);
    }
    std::uint64_t sequence = 0;
    auto observe = [&](AttemptState state, bool retryable = false,
                       int exit = 0) {
      const Observation observation{attempt.id, node.id, attempt.generation,
                                   ++sequence, state, retryable, exit};
      if (journal) {
        const auto recorded = durable.record_observation(observation);
        if (!recorded.ok()) {
          return recorded;
        }
      }
      return controller.observe(observation);
    };
    ChildProcess child;
    status = runtime.launch(worker.container_id, bundle, child);
    if (!status.ok()) {
      // Even setup failure after spawn may leave a container: only a failed
      // spawn (no child) proves this attempt never reached the backend.
      const auto state = child.result().finished ? AttemptState::unknown
                                                 : AttemptState::failed;
      observe(state, false, state == AttemptState::failed ? 127 : 0);
      worker.state = state;
      worker.process = child.result();
      worker.cleanup = status;
      result.workers.push_back(std::move(worker));
      return finish(status);
    }
    auto next_state = MonotonicClock::now();
    TimePoint force_at{}, abandon_at{};
    bool stopping = false, forced = false, running = false;
    while (!child.result().finished) {
      status = child.poll();
      if (!status.ok()) {
        break;
      }
      if (child.result().finished) {
        break;
      }
      auto now = MonotonicClock::now();
      if (!stopping && (now >= deadline || (cancelled && cancelled()))) {
        session_cancelled = true;
        stopping = true;
        status = note_cancellation();
        if (status.ok()) {
          status = controller.cancel(workload_id, workload.tenant);
        }
        if (!status.ok()) {
          break;
        }
        force_at = now + std::chrono::milliseconds(options.grace_millis);
        abandon_at = force_at + std::chrono::seconds(3);
        runtime.signal(worker.container_id, false);
      }
      now = MonotonicClock::now();
      if (stopping && !forced && now >= force_at) {
        runtime.signal(worker.container_id, true);
        forced = true;
      }
      if (now >= next_state) {
        RuntimeState observed;
        const auto query = runtime.state(worker.container_id, observed);
        next_state = MonotonicClock::now() + std::chrono::milliseconds(100);
        if (query.ok() && observed.status == "running" && !running) {
          status = observe(AttemptState::running);
          running = true;
          if (!status.ok()) {
            break;
          }
        }
        if (stopping && query.ok() && observed.status == "stopped") {
          // The container stopped, but its runtime monitor may still be
          // exiting.
          child.poll();
          if (!child.result().finished && MonotonicClock::now() >= abandon_at) {
            child.terminate();
          }
        } else if (stopping && MonotonicClock::now() >= abandon_at) {
          child.terminate();
          break;
        }
      }
      if (stopping && MonotonicClock::now() >= abandon_at) {
        child.terminate();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // An exited helper is insufficient evidence. Retain the allocation if the
    // backend cannot report a matching stopped container.
    RuntimeState observed;
    const auto query = runtime.state(worker.container_id, observed);
    if (!query.ok() || observed.status != "stopped") {
      child.terminate();
      child.poll();
      observe(AttemptState::unknown);
      worker.state = AttemptState::unknown;
      worker.process = child.result();
      worker.cleanup = {StatusCode::unavailable,
                        "container termination is unconfirmed; runtime state "
                        "and reservation retained"};
      result.workers.push_back(std::move(worker));
      return finish(
          {StatusCode::unavailable, "container termination is unconfirmed; "
                                    "inspect the session runtime state"});
    }
    child.poll();
    worker.process = child.result();
    worker.cleanup = runtime.remove(worker.container_id);
    cleanup_failed = cleanup_failed || !worker.cleanup.ok();
    worker.state = stopping ? AttemptState::cancelled
                   : worker.process.exited && worker.process.exit_code == 0
                       ? AttemptState::succeeded
                       : AttemptState::failed;
    const int exit = worker.state == AttemptState::failed
                         ? (worker.process.exited
                                ? worker.process.exit_code
                                : 128 + std::min(worker.process.signal, 127))
                         : 0;
    // Normal nonzero attached exits use the explicitly declared retry budget.
    // Sampling may miss Running for short tasks; it must not change retry
    // policy.
    status = observe(
        worker.state,
        worker.state == AttemptState::failed && worker.process.exited, exit);
    result.workers.push_back(std::move(worker));
    if (!status.ok()) {
      return finish(status);
    }
    if (cleanup_failed) {
      status = note_cancellation();
      if (status.ok()) {
        status = controller.cancel(workload_id, workload.tenant);
      }
      if (!status.ok()) {
        return finish(status);
      }
      return finish(
          {StatusCode::unavailable, "container stopped; cleanup failed and "
                                    "further execution was cancelled"});
    }
  }
}
} // namespace omnimesh

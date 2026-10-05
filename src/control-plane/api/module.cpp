#include "omnimesh/control_plane.hpp"
#include <cerrno>
#include <nlohmann/json.hpp>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace omnimesh {
namespace {
using Json = nlohmann::json;
struct RecordError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void require(bool condition) {
  if (!condition) {
    throw RecordError("invalid durable record");
  }
}
void fields(const Json &value, std::initializer_list<const char *> keys) {
  require(value.is_object() && value.size() == keys.size());
  for (const auto *key : keys) {
    require(value.contains(key));
  }
}
std::string text(const Json &value) {
  require(value.is_string());
  const auto result = value.get<std::string>();
  require(!result.empty() && result.size() <= 256 &&
          result.find('\0') == std::string::npos);
  return result;
}
std::uint64_t number(const Json &value, std::uint64_t maximum = UINT64_MAX) {
  require(value.is_number_unsigned());
  const auto result = value.get<std::uint64_t>();
  require(result <= maximum);
  return result;
}
Json decode(std::uint8_t type, const std::string &payload) {
  std::size_t events = 0;
  std::vector<std::set<std::string>> keys;
  const auto callback = [&](int depth, Json::parse_event_t event, Json &value) {
    require(depth <= 32 && ++events <= 16384);
    if (event == Json::parse_event_t::object_start) {
      keys.emplace_back();
    } else if (event == Json::parse_event_t::object_end) {
      keys.pop_back();
    } else if (event == Json::parse_event_t::key) {
      require(keys.back().insert(value.get<std::string>()).second);
    }
    return true;
  };
  const auto envelope = Json::parse(payload, callback);
  fields(envelope, {"t", "v"});
  require(number(envelope["t"], 255) == type);
  const auto &value = envelope["v"];
  switch (static_cast<RecordType>(type)) {
  case RecordType::node_inventory:
    require(parse_node(value.dump()).status.ok());
    break;
  case RecordType::workload_desired:
    require(parse_workload(value.dump()).status.ok());
    break;
  case RecordType::tenant_quota: {
    fields(value, {"tenant", "cpuMillis", "memoryBytes", "maxAllocations"});
    PlacementRequirements requirements;
    requirements.tenant = text(value["tenant"]);
    requirements.resources = {number(value["cpuMillis"]),
                              number(value["memoryBytes"])};
    require(validate_requirements(requirements).ok());
    require(number(value["maxAllocations"], kMaxAllocations) > 0);
    break;
  }
  case RecordType::workload_cancelled: {
    fields(value, {"workloadId", "tenant"});
    const auto id = text(value["workloadId"]), tenant = text(value["tenant"]);
    const auto slash = id.find('/');
    require(slash != std::string::npos && id.substr(0, slash) == tenant &&
            id.find('/', slash + 1) == std::string::npos);
    auto workload = Workload{};
    workload.name = id.substr(slash + 1);
    workload.tenant = tenant;
    workload.image = "sha256:" + std::string(64, 'a');
    workload.command = {"/validate"};
    require(validate_workload(workload).empty());
    break;
  }
  case RecordType::reservation: {
    fields(value,
           {"taskId", "nodeId", "attemptNumber", "generation", "tenant"});
    const auto task = text(value["taskId"]), tenant = text(value["tenant"]);
    require(task.compare(0, tenant.size() + 1, tenant + "/") == 0);
    text(value["nodeId"]);
    require(number(value["attemptNumber"], 16) > 0 &&
            number(value["generation"]) > 0);
    break;
  }
  case RecordType::attempt_observed: {
    fields(value, {"attemptId", "nodeId", "generation", "sequence", "state",
                   "retryable", "exitCode"});
    text(value["attemptId"]);
    text(value["nodeId"]);
    require(number(value["generation"]) > 0 && number(value["sequence"]) > 0);
    const auto state = number(
        value["state"], static_cast<std::uint32_t>(AttemptState::cancelled));
    require(state >= static_cast<std::uint32_t>(AttemptState::starting) &&
            value["retryable"].is_boolean());
    const auto exit = number(value["exitCode"], 255);
    require(state == static_cast<std::uint32_t>(AttemptState::failed) ||
            (exit == 0 && !value["retryable"].get<bool>()));
    break;
  }
  case RecordType::task_resolved: {
    fields(value, {"taskId", "tenant", "state"});
    const auto task = text(value["taskId"]), tenant = text(value["tenant"]);
    require(task.compare(0, tenant.size() + 1, tenant + "/") == 0);
    const auto state = number(
        value["state"], static_cast<std::uint32_t>(TaskState::cancelled));
    require(state == static_cast<std::uint32_t>(TaskState::succeeded) ||
            state == static_cast<std::uint32_t>(TaskState::failed) ||
            state == static_cast<std::uint32_t>(TaskState::cancelled));
    break;
  }
  default:
    throw RecordError("unsupported durable record type");
  }
  return value;
}
std::string envelope(RecordType type, const Json &value) {
  return Json({{"t", static_cast<std::uint32_t>(type)}, {"v", value}}).dump();
}
void accepted(const Status &status) {
  if (!status.ok()) {
    throw RecordError("durable record conflicts with recovered state");
  }
}
} // namespace
Status DurableControlPlane::open(const std::string &directory) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (directory.empty() || directory.size() > 4096 ||
      directory.find('\0') != std::string::npos) {
    return {StatusCode::invalid_argument, "invalid state directory"};
  }
  if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
    return {StatusCode::unavailable, "cannot create private state directory"};
  }
  struct stat info{};
  if (lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
      info.st_uid != getuid() || (info.st_mode & 0777) != 0700) {
    return {StatusCode::permission_denied,
            "state directory must be private and owned by this user"};
  }
  directory_ = directory;
  return journal_.open(directory + "/control-plane.journal");
}
Status DurableControlPlane::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  return journal_.close();
}
RecoveryReport DurableControlPlane::recover(Allocator &allocator,
                                            WorkloadController &controller) {
  std::lock_guard<std::mutex> lock(mutex_);
  RecoveryReport report;
  // Validate all payloads before mutating the caller's state. A later ordering
  // conflict still fails recovery; callers must discard that partial state.
  auto status =
      journal_.replay([](std::uint8_t type, const std::string &payload) {
        decode(type, payload);
      });
  if (status.ok()) {
    status = journal_.replay([&](std::uint8_t raw, const std::string &payload) {
      const auto value = decode(raw, payload);
      switch (static_cast<RecordType>(raw)) {
      case RecordType::node_inventory:
        accepted(allocator.upsert_node(parse_node(value.dump()).value));
        break;
      case RecordType::tenant_quota:
        accepted(allocator.set_quota(
            value["tenant"],
            {{number(value["cpuMillis"]), number(value["memoryBytes"])},
             static_cast<std::size_t>(number(value["maxAllocations"]))}));
        break;
      case RecordType::workload_desired: {
        const auto workload = parse_workload(value.dump()).value;
        accepted(controller.submit(workload, workload.tenant));
        break;
      }
      case RecordType::workload_cancelled:
        accepted(controller.cancel(value["workloadId"], value["tenant"]));
        break;
      case RecordType::reservation:
        accepted(controller.adopt_reservation(
            value["taskId"], value["nodeId"],
            static_cast<std::uint32_t>(number(value["attemptNumber"])),
            number(value["generation"]), value["tenant"]));
        ++report.reservations_recovered;
        break;
      case RecordType::task_resolved:
        accepted(controller.adopt_resolution(
            value["taskId"], static_cast<TaskState>(number(value["state"])),
            value["tenant"]));
        break;
      case RecordType::attempt_observed: {
        auto state = static_cast<AttemptState>(number(value["state"]));
        // A historic liveness observation cannot establish present liveness.
        if (state == AttemptState::starting || state == AttemptState::running ||
            state == AttemptState::unknown) {
          state = AttemptState::unknown;
        }
        Observation observation{text(value["attemptId"]),
                                text(value["nodeId"]),
                                number(value["generation"]),
                                number(value["sequence"]),
                                state,
                                value["retryable"].get<bool>(),
                                static_cast<int>(number(value["exitCode"]))};
        const auto first = observation.attempt_id.find('/'),
                   second = observation.attempt_id.find('/', first + 1);
        require(first != std::string::npos && second != std::string::npos);
        WorkloadRecord workload;
        accepted(controller.inspect(observation.attempt_id.substr(0, second),
                                    observation.attempt_id.substr(0, first),
                                    workload));
        bool already_applied = false;
        for (const auto &task : workload.tasks) {
          for (const auto &attempt : task.attempts) {
            if (attempt.id == observation.attempt_id &&
                attempt.observation_sequence > observation.sequence) {
              already_applied = true;
            }
          }
        }
        if (already_applied) {
          ++report.records_skipped;
        } else {
          accepted(controller.observe(observation));
          ++report.observations_recovered;
        }
        break;
      }
      }
      ++report.records_applied;
    });
  }
  report.diagnostics = journal_.diagnostics();
  if (!status.ok()) {
    report.diagnostics.push_back({"$journal", status.message});
  }
  report.status = status.ok();
  return report;
}
Status DurableControlPlane::record_node(const Node &node) {
  return append(
      static_cast<std::uint8_t>(RecordType::node_inventory),
      envelope(RecordType::node_inventory, Json::parse(encode_node(node))));
}
Status DurableControlPlane::record_quota(const std::string &tenant,
                                         const TenantQuota &quota) {
  return append(static_cast<std::uint8_t>(RecordType::tenant_quota),
                envelope(RecordType::tenant_quota,
                         {{"tenant", tenant},
                          {"cpuMillis", quota.limit.cpu_millis},
                          {"memoryBytes", quota.limit.memory_bytes},
                          {"maxAllocations", quota.max_allocations}}));
}
Status DurableControlPlane::record_workload(const Workload &workload) {
  return append(static_cast<std::uint8_t>(RecordType::workload_desired),
                envelope(RecordType::workload_desired,
                         Json::parse(encode_workload(workload))));
}
Status DurableControlPlane::record_cancellation(const std::string &id) {
  return append(
      static_cast<std::uint8_t>(RecordType::workload_cancelled),
      envelope(RecordType::workload_cancelled,
               {{"workloadId", id}, {"tenant", id.substr(0, id.find('/'))}}));
}
Status DurableControlPlane::record_reservation(const std::string &task,
                                               const std::string &node,
                                               std::uint32_t attempt,
                                               std::uint64_t generation) {
  return append(static_cast<std::uint8_t>(RecordType::reservation),
                envelope(RecordType::reservation,
                         {{"taskId", task},
                          {"nodeId", node},
                          {"attemptNumber", attempt},
                          {"generation", generation},
                          {"tenant", task.substr(0, task.find('/'))}}));
}
Status DurableControlPlane::record_observation(const Observation &observation) {
  return append(
      static_cast<std::uint8_t>(RecordType::attempt_observed),
      envelope(RecordType::attempt_observed,
               {{"attemptId", observation.attempt_id},
                {"nodeId", observation.node_id},
                {"generation", observation.generation},
                {"sequence", observation.sequence},
                {"state", static_cast<std::uint32_t>(observation.state)},
                {"retryable", observation.retryable},
                {"exitCode", observation.exit_code}}));
}
Status DurableControlPlane::append(std::uint8_t type,
                                   const std::string &payload) {
  try {
    decode(type, payload);
  } catch (const std::exception &) {
    return {StatusCode::invalid_argument, "invalid durable record fields"};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return journal_.append(type, payload);
}
Status DurableControlPlane::compact(const Allocator &allocator,
                                    const WorkloadController &controller) {
  std::vector<WorkloadRecord> workloads;
  const auto status = controller.live_state(workloads);
  if (!status.ok()) {
    return status;
  }
  std::vector<Allocation> allocations;
  const auto reserved = allocator.active_allocations(allocations);
  if (!reserved.ok()) {
    return reserved;
  }
  // Every active allocation must correspond to an unfinished attempt, otherwise
  // compaction would silently drop committed capacity from recovery.
  std::size_t unfinished = 0;
  for (const auto &record : workloads) {
    for (const auto &task : record.tasks) {
      const auto resolved = task.state == TaskState::succeeded ||
                            task.state == TaskState::failed ||
                            task.state == TaskState::cancelled;
      if (!resolved) {
        continue;
      }
      // A resolved task must hold no capacity: its attempts are replayed and
      // released, so a lingering allocation would mean accounting disagreement.
      for (const auto &attempt : task.attempts) {
        if (attempt.state != AttemptState::succeeded &&
            attempt.state != AttemptState::failed &&
            attempt.state != AttemptState::cancelled) {
          return {StatusCode::conflict,
                  "refusing to compact: a resolved task still has an "
                  "unfinished attempt"};
        }
      }
    }
    for (const auto &task : record.tasks) {
      const auto resolved = task.state == TaskState::succeeded ||
                            task.state == TaskState::failed ||
                            task.state == TaskState::cancelled;
      if (!resolved) {
        for (const auto &attempt : task.attempts) {
          if (attempt.state != AttemptState::succeeded &&
              attempt.state != AttemptState::failed &&
              attempt.state != AttemptState::cancelled) {
            ++unfinished;
          }
        }
      }
    }
  }
  if (allocations.size() != unfinished) {
    return {StatusCode::conflict,
            "refusing to compact: allocation accounting and attempt history "
            "disagree; compacting could drop committed capacity"};
  }
  // Order matters: quotas before workloads, workloads before reservations, and
  // reservations before observations, because each record is applied in
  // sequence during recovery.
  std::vector<std::pair<std::uint8_t, std::string>> records;
  const auto push = [&records](RecordType type, const Json &value) {
    records.emplace_back(static_cast<std::uint8_t>(type), envelope(type, value));
  };
  const auto snapshot = allocator.snapshot();
  for (const auto &entry : snapshot.tenants) {
    push(RecordType::tenant_quota,
         {{"tenant", entry.first},
          {"cpuMillis", entry.second.quota.limit.cpu_millis},
          {"memoryBytes", entry.second.quota.limit.memory_bytes},
          {"maxAllocations", entry.second.quota.max_allocations}});
  }
  for (const auto &entry : snapshot.nodes) {
    push(RecordType::node_inventory, Json::parse(encode_node(entry.second.node)));
  }
  for (const auto &record : workloads) {
    push(RecordType::workload_desired, Json::parse(encode_workload(record.spec)));
    if (record.cancel_requested) {
      push(RecordType::workload_cancelled,
           {{"workloadId", record.spec.tenant + "/" + record.spec.name},
            {"tenant", record.spec.tenant}});
    }
  }
  // Attempt ordering belongs to the controller, so reservation records come from
  // its attempt history rather than from allocation identity strings.
  // adopt_reservation restores an unfinished attempt as Unknown, which is the
  // honest post-compaction state: a recorded liveness event cannot establish
  // present liveness, so compaction must not imply a worker is running.
  //
  // Attempt history is preserved in order: reservation, then the attempt's final
  // observation. Replay re-reserves transiently and the terminal observation
  // releases it again, so accounting is unchanged and no attempt is lost. A
  // terminal task_state follows, because adopt_reservation only accepts a task
  // that is not already resolved.
  for (const auto &record : workloads) {
    for (const auto &task : record.tasks) {
      for (const auto &attempt : task.attempts) {
        push(RecordType::reservation,
             {{"taskId", task.id},
              {"nodeId", attempt.node_id},
              {"attemptNumber", attempt.number},
              {"generation", attempt.generation},
              {"tenant", record.spec.tenant}});
        if (attempt.state == AttemptState::succeeded ||
            attempt.state == AttemptState::failed ||
            attempt.state == AttemptState::cancelled) {
          push(RecordType::attempt_observed,
               {{"attemptId", attempt.id},
                {"nodeId", attempt.node_id},
                {"generation", attempt.generation},
                {"sequence", attempt.observation_sequence},
                {"state", static_cast<std::uint32_t>(attempt.state)},
                {"retryable", attempt.retryable},
                {"exitCode", attempt.exit_code}});
        }
      }
      if (task.state == TaskState::succeeded || task.state == TaskState::failed ||
          task.state == TaskState::cancelled) {
        push(RecordType::task_resolved,
             {{"taskId", task.id},
              {"tenant", record.spec.tenant},
              {"state", static_cast<std::uint32_t>(task.state)}});
      }
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return journal_.compact(records);
}
JournalStats DurableControlPlane::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return journal_.stats();
}
std::vector<Diagnostic> DurableControlPlane::diagnostics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return journal_.diagnostics();
}
} // namespace omnimesh

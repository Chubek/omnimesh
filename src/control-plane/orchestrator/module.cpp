#include "omnimesh/orchestrator.hpp"
#include "omnimesh/manifest.hpp"

#include <algorithm>

namespace omnimesh {
namespace {

bool terminal(AttemptState state) {
  return state == AttemptState::succeeded || state == AttemptState::failed ||
         state == AttemptState::cancelled;
}

bool terminal(TaskState state) {
  return state == TaskState::succeeded || state == TaskState::failed ||
         state == TaskState::cancelled;
}

bool transition_allowed(AttemptState from, AttemptState to) {
  if (from == AttemptState::allocated || terminal(from) || to == AttemptState::allocated) {
    return false;
  }
  if ((from == AttemptState::running || from == AttemptState::unknown) &&
      to == AttemptState::starting) {
    return false;
  }
  return true;
}

std::chrono::milliseconds retry_delay(const RetryPolicy& policy,
                                      const AttemptRecord& attempt) {
  std::uint64_t delay = policy.backoff_millis;
  for (std::uint32_t i = 1; i < attempt.number; ++i) {
    delay = std::min(std::uint64_t{60000}, delay * 2);
  }
  // Stable per-attempt jitter, independent of process-global random state.
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : attempt.id) {
    hash = (hash ^ byte) * 1099511628211ULL;
  }
  delay = std::min(std::uint64_t{60000}, delay + hash % (delay / 4 + 1));
  return std::chrono::milliseconds(delay);
}

} // namespace

const char* task_state_name(TaskState state) noexcept {
  switch (state) {
  case TaskState::pending: return "Pending";
  case TaskState::queued: return "Queued";
  case TaskState::allocated: return "Allocated";
  case TaskState::starting: return "Starting";
  case TaskState::running: return "Running";
  case TaskState::unknown: return "Unknown";
  case TaskState::cancelling: return "Cancelling";
  case TaskState::succeeded: return "Succeeded";
  case TaskState::failed: return "Failed";
  case TaskState::cancelled: return "Cancelled";
  }
  return "Unknown";
}

const char* attempt_state_name(AttemptState state) noexcept {
  switch (state) {
  case AttemptState::allocated: return "Allocated";
  case AttemptState::starting: return "Starting";
  case AttemptState::running: return "Running";
  case AttemptState::unknown: return "Unknown";
  case AttemptState::succeeded: return "Succeeded";
  case AttemptState::failed: return "Failed";
  case AttemptState::cancelled: return "Cancelled";
  }
  return "Unknown";
}

Status WorkloadController::submit(Workload workload, const std::string& tenant) {
  if (tenant != workload.tenant) {
    return {StatusCode::permission_denied, "caller tenant does not match workload tenant"};
  }
  const auto diagnostics = validate_workload(workload);
  if (!diagnostics.empty()) {
    return {StatusCode::invalid_argument,
            diagnostics.front().path + ": " + diagnostics.front().message};
  }
  std::sort(workload.capabilities.begin(), workload.capabilities.end());
  const auto id = workload.tenant + "/" + workload.name;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto previous = workloads_.find(id);
  if (previous != workloads_.end()) {
    return previous->second.spec == workload
               ? Status::Ok()
               : Status{StatusCode::conflict,
                        "workload identity already exists with a different specification; updates are unsupported"};
  }
  if (workloads_.size() >= kMaxWorkloads || workload.replicas > kMaxTasks - task_count_) {
    return {StatusCode::resource_exhausted, "workload or task history limit reached"};
  }
  if (allocator_.snapshot().tenants.count(tenant) == 0) {
    return {StatusCode::permission_denied, "tenant has no configured quota"};
  }
  WorkloadRecord record;
  record.spec = std::move(workload);
  record.tasks.reserve(record.spec.replicas);
  for (std::uint32_t replica = 0; replica < record.spec.replicas; ++replica) {
    TaskRecord task;
    task.id = id + "/g" + std::to_string(record.spec.generation) +
              "/r" + std::to_string(replica);
    task.replica = replica;
    task.attempts.reserve(record.spec.retry.max_attempts);
    record.tasks.push_back(std::move(task));
  }
  const auto count = record.tasks.size();
  workloads_.emplace(id, std::move(record));
  task_count_ += count;
  return Status::Ok();
}

Status WorkloadController::reconcile(TimePoint now) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : workloads_) {
    auto& record = entry.second;
    for (auto& task : record.tasks) {
      if (task.cancel_requested || terminal(task.state)) {
        continue;
      }
      if (task.state == TaskState::pending) {
        task.state = TaskState::queued;
      }
      if (task.state != TaskState::queued || now < task.eligible_at) {
        continue;
      }
      const auto requirements = requirements_for(record.spec);
      task.placement = propose_placement(requirements, allocator_.snapshot());
      if (!task.placement.status.ok()) {
        continue;
      }
      AttemptRecord attempt;
      attempt.number = static_cast<std::uint32_t>(task.attempts.size()) + 1;
      attempt.generation = record.spec.generation;
      attempt.id = task.id + "/a" + std::to_string(attempt.number);
      attempt.allocation_id = "alloc/" + attempt.id;
      task.attempts.push_back(std::move(attempt));
      auto& current = task.attempts.back();
      try {
        for (const auto& node_id : task.placement.candidates) {
          current.node_id = node_id;
          ReservationRequest request{entry.first, task.id, current.id,
                                     current.generation, node_id, requirements};
          Allocation allocation;
          auto status = allocator_.reserve(request, allocation);
          if (status.ok()) {
            task.state = TaskState::allocated;
            break;
          }
          task.placement.status = std::move(status);
        }
      } catch (...) {
        task.attempts.pop_back();
        throw;
      }
      if (task.state != TaskState::allocated) {
        task.attempts.pop_back();
      } else {
        // Node diagnostics are retained only for queued tasks.
        task.placement = {};
      }
    }
  }
  return Status::Ok();
}

Status WorkloadController::inspect(const std::string& workload_id,
                                   const std::string& tenant,
                                   WorkloadRecord& record) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = workloads_.find(workload_id);
  if (found == workloads_.end()) {
    return {StatusCode::not_found, "workload not found"};
  }
  if (found->second.spec.tenant != tenant) {
    return {StatusCode::permission_denied, "workload belongs to another tenant"};
  }
  record = found->second;
  return Status::Ok();
}

WorkloadController::AttemptLocation WorkloadController::find_attempt(const std::string& id) {
  for (auto& entry : workloads_) {
    for (auto& task : entry.second.tasks) {
      for (auto& attempt : task.attempts) {
        if (attempt.id == id) {
          return {&entry.second, &task, &attempt};
        }
      }
    }
  }
  return {};
}

bool WorkloadController::find_task(const std::string& task_id, WorkloadRecord** workload,
                                   TaskRecord** task) {
  for (auto& entry : workloads_) {
    for (auto& candidate : entry.second.tasks) {
      if (candidate.id == task_id) {
        *workload = &entry.second;
        *task = &candidate;
        return true;
      }
    }
  }
  return false;
}

Status WorkloadController::adopt_reservation(const std::string& task_id,
                                            const std::string& node_id,
                                            std::uint32_t attempt_number,
                                            std::uint64_t generation,
                                            const std::string& tenant) {
  if (attempt_number == 0 || attempt_number > 16 ||
      generation == 0 || node_id.empty() || node_id.size() > 256) {
    return {StatusCode::invalid_argument, "invalid recovered reservation"};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  WorkloadRecord* workload = nullptr;
  TaskRecord* task = nullptr;
  if (!find_task(task_id, &workload, &task)) {
    return {StatusCode::not_found, "task not found"};
  }
  if (workload->spec.tenant != tenant) {
    return {StatusCode::permission_denied, "task belongs to another tenant"};
  }
  if (generation != workload->spec.generation) {
    return {StatusCode::conflict, "recovered reservation targets a stale generation"};
  }
  const std::string attempt_id = task_id + "/a" + std::to_string(attempt_number);
  for (const auto& attempt : task->attempts) {
    if (attempt.id == attempt_id) {
      return attempt.node_id == node_id
                 ? Status::Ok()
                 : Status{StatusCode::conflict,
                          "attempt identity already exists on another node"};
    }
  }
  if (task->cancel_requested || terminal(task->state)) {
    return {StatusCode::conflict, "task is already resolved and cannot adopt an attempt"};
  }
  if (attempt_number != task->attempts.size() + 1 ||
      attempt_number > workload->spec.retry.max_attempts ||
      (!task->attempts.empty() && !terminal(task->attempts.back().state))) {
    return {StatusCode::conflict, "recovered attempts violate retry order or budget"};
  }
  const auto requirements = requirements_for(workload->spec);
  ReservationRequest request{workload->spec.tenant + "/" + workload->spec.name,
                             task_id, attempt_id, generation, node_id, requirements};
  AttemptRecord attempt;
  attempt.id = attempt_id;
  attempt.number = attempt_number;
  attempt.generation = generation;
  attempt.node_id = node_id;
  attempt.allocation_id = "alloc/" + attempt_id;
  attempt.state = AttemptState::unknown;
  task->attempts.push_back(std::move(attempt));
  Allocation allocation;
  Status status;
  try { status = allocator_.reserve(request, allocation); }
  catch (...) { task->attempts.pop_back(); throw; }
  if (!status.ok()) { task->attempts.pop_back(); return status; }
  task->state = TaskState::unknown;
  return Status::Ok();
}

Status WorkloadController::begin_start(const std::string& attempt_id,
                                       const std::string& tenant) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto location = find_attempt(attempt_id);
  if (!location.attempt) {
    return {StatusCode::not_found, "attempt not found"};
  }
  if (location.workload->spec.tenant != tenant) {
    return {StatusCode::permission_denied, "attempt belongs to another tenant"};
  }
  auto& task = *location.task;
  auto& attempt = *location.attempt;
  if (task.cancel_requested) {
    return {StatusCode::conflict, "attempt has a cancellation request"};
  }
  if (&attempt != &task.attempts.back()) {
    return {StatusCode::conflict, "attempt has been superseded"};
  }
  if (attempt.state == AttemptState::starting || attempt.state == AttemptState::running) {
    return Status::Ok();
  }
  if (attempt.state != AttemptState::allocated) {
    return {StatusCode::conflict, "attempt cannot be started in its observed state"};
  }
  Allocation allocation;
  const auto status = allocator_.inspect(attempt.allocation_id, tenant, allocation);
  if (!status.ok()) {
    return status;
  }
  if (!allocation.active) {
    return {StatusCode::conflict, "attempt allocation is no longer active"};
  }
  auto snapshot = allocator_.snapshot();
  auto node = snapshot.nodes.find(attempt.node_id);
  if (node == snapshot.nodes.end()) {
    return {StatusCode::unavailable, "allocated node is no longer registered"};
  }
  // Recheck changed readiness/labels before issuing a start intent, excluding
  // this attempt's own already-accounted reservation from the fit calculation.
  const auto resources = allocation.request.requirements.resources;
  if (node->second.used.cpu_millis < resources.cpu_millis ||
      node->second.used.memory_bytes < resources.memory_bytes) {
    return {StatusCode::internal, "allocation accounting is inconsistent"};
  }
  node->second.used.cpu_millis -= resources.cpu_millis;
  node->second.used.memory_bytes -= resources.memory_bytes;
  const auto reasons = node_rejections(allocation.request.requirements, node->second);
  if (!reasons.empty()) {
    return {StatusCode::unavailable, reasons.front()};
  }
  attempt.state = AttemptState::starting;
  task.state = TaskState::starting;
  return Status::Ok();
}

Status WorkloadController::observe(const Observation& observation, TimePoint now) {
  if (observation.sequence == 0 || observation.exit_code < 0 || observation.exit_code > 255 ||
      (observation.retryable && observation.state != AttemptState::failed) ||
      (observation.state != AttemptState::failed && observation.exit_code != 0)) {
    return {StatusCode::invalid_argument, "invalid observation sequence, exit status or retry classification"};
  }
  switch (observation.state) {
  case AttemptState::starting:
  case AttemptState::running:
  case AttemptState::unknown:
  case AttemptState::succeeded:
  case AttemptState::failed:
  case AttemptState::cancelled: break;
  default: return {StatusCode::invalid_argument, "unsupported observed attempt state"};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto location = find_attempt(observation.attempt_id);
  if (!location.attempt) {
    return {StatusCode::not_found, "attempt not found"};
  }
  auto& attempt = *location.attempt;
  auto& task = *location.task;
  const auto& workload = location.workload->spec;
  if (observation.node_id != attempt.node_id) {
    return {StatusCode::permission_denied, "observation does not belong to the allocated node"};
  }
  if (observation.generation != attempt.generation) {
    return {StatusCode::conflict, "stale desired-state generation"};
  }
  const bool same = observation.state == attempt.state &&
                    observation.retryable == attempt.retryable &&
                    observation.exit_code == attempt.exit_code;
  if (observation.sequence < attempt.observation_sequence) {
    return {StatusCode::conflict, "stale observation sequence"};
  }
  if (observation.sequence == attempt.observation_sequence) {
    return same ? Status::Ok()
                : Status{StatusCode::conflict, "observation sequence was reused with different content"};
  }
  if (terminal(attempt.state)) {
    if (!same) {
      return {StatusCode::conflict, "terminal attempt observations cannot be changed"};
    }
    attempt.observation_sequence = observation.sequence;
    return Status::Ok();
  }
  if (!transition_allowed(attempt.state, observation.state)) {
    return {StatusCode::conflict, "attempt observation would violate lifecycle ordering"};
  }
  const bool retry = observation.state == AttemptState::failed && observation.retryable &&
                     !task.cancel_requested && attempt.number < workload.retry.max_attempts;
  TimePoint eligible_at{};
  if (retry) {
    const auto delay = std::chrono::duration_cast<MonotonicClock::duration>(
        retry_delay(workload.retry, attempt));
    if (now > TimePoint::max() - delay) {
      return {StatusCode::invalid_argument, "retry deadline exceeds the monotonic clock range"};
    }
    eligible_at = now + delay;
  }
  if (terminal(observation.state)) {
    const auto status = allocator_.release(attempt.allocation_id, workload.tenant);
    if (!status.ok()) {
      return status;
    }
  }
  attempt.state = observation.state;
  attempt.observation_sequence = observation.sequence;
  attempt.retryable = observation.retryable;
  attempt.exit_code = observation.exit_code;
  if (terminal(observation.state)) {
    if (task.cancel_requested || observation.state == AttemptState::cancelled) {
      task.state = TaskState::cancelled;
    } else if (observation.state == AttemptState::succeeded) {
      task.state = TaskState::succeeded;
    } else if (retry) {
      task.state = TaskState::queued;
      task.eligible_at = eligible_at;
    } else {
      task.state = TaskState::failed;
    }
  } else if (task.cancel_requested) {
    task.state = TaskState::cancelling;
  } else {
    switch (observation.state) {
    case AttemptState::starting: task.state = TaskState::starting; break;
    case AttemptState::running: task.state = TaskState::running; break;
    default: task.state = TaskState::unknown; break;
    }
  }
  return Status::Ok();
}

Status WorkloadController::cancel(const std::string& workload_id,
                                 const std::string& tenant) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = workloads_.find(workload_id);
  if (found == workloads_.end()) {
    return {StatusCode::not_found, "workload not found"};
  }
  auto& record = found->second;
  if (record.spec.tenant != tenant) {
    return {StatusCode::permission_denied, "workload belongs to another tenant"};
  }
  record.cancel_requested = true;
  for (auto& task : record.tasks) {
    if (terminal(task.state)) {
      continue;
    }
    task.cancel_requested = true;
    if (task.state == TaskState::pending || task.state == TaskState::queued) {
      task.state = TaskState::cancelled;
    } else if (task.state == TaskState::allocated) {
      auto& attempt = task.attempts.back();
      const auto status = allocator_.release(attempt.allocation_id, tenant);
      if (!status.ok()) {
        return status;
      }
      attempt.state = AttemptState::cancelled;
      task.state = TaskState::cancelled;
    } else {
      // Starting/Running/Unknown might still be producing external effects.
      // Retain reservations until the adapter reports confirmed termination.
      task.state = TaskState::cancelling;
    }
  }
  return Status::Ok();
}

} // namespace omnimesh

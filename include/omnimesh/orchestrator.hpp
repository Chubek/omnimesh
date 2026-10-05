#pragma once

#include "omnimesh/scheduler.hpp"

#include <chrono>

namespace omnimesh {

using MonotonicClock = std::chrono::steady_clock;
using TimePoint = MonotonicClock::time_point;

enum class TaskState {
  pending, queued, allocated, starting, running, unknown, cancelling,
  succeeded, failed, cancelled
};
enum class AttemptState {
  allocated, starting, running, unknown, succeeded, failed, cancelled
};

const char* task_state_name(TaskState state) noexcept;
const char* attempt_state_name(AttemptState state) noexcept;

struct AttemptRecord {
  std::string id;
  std::uint32_t number{0};
  std::uint64_t generation{0};
  std::string node_id;
  std::string allocation_id;
  AttemptState state{AttemptState::allocated};
  std::uint64_t observation_sequence{0};
  bool retryable{false};
  int exit_code{0};
};

struct TaskRecord {
  std::string id;
  std::uint32_t replica{0};
  TaskState state{TaskState::pending};
  bool cancel_requested{false};
  TimePoint eligible_at{};
  std::vector<AttemptRecord> attempts;
  PlacementResult placement;
};

struct WorkloadRecord {
  Workload spec;
  bool cancel_requested{false};
  std::vector<TaskRecord> tasks;
};

struct Observation {
  std::string attempt_id;
  std::string node_id;
  std::uint64_t generation{0};
  std::uint64_t sequence{0};
  AttemptState state{AttemptState::unknown};
  bool retryable{false};
  int exit_code{0};
};

// Process-local experimental control plane. Caller identities are trusted input
// from a future authenticated API/agent adapter; this class is not a network API.
// The allocator must outlive the controller. Lock order: controller -> allocator.
class WorkloadController {
public:
  explicit WorkloadController(Allocator& allocator) : allocator_(allocator) {}

  Status submit(Workload workload, const std::string& tenant);
  Status reconcile(TimePoint now = MonotonicClock::now());
  Status inspect(const std::string& workload_id, const std::string& tenant,
                 WorkloadRecord& record) const;
  Status begin_start(const std::string& attempt_id, const std::string& tenant);
  Status observe(const Observation& observation,
                 TimePoint now = MonotonicClock::now());
  Status cancel(const std::string& workload_id, const std::string& tenant);

  // Rebuilds accounting for a recorded reservation. Recovered attempts are
  // Unknown: absence of a durable start acknowledgment cannot prove no worker
  // started. This never issues a new start intent. Identical identities are
  // idempotent, and retries must follow the recorded attempt order/budget.
  Status adopt_reservation(const std::string& task_id, const std::string& node_id,
                           std::uint32_t attempt_number, std::uint64_t generation,
                           const std::string& tenant);

  // Records a final task outcome with no attempt attached. Only Succeeded,
  // Failed and Cancelled are accepted, the task must be resolvable, and it must
  // not already hold an unfinished attempt. Used to preserve resolved tasks
  // across compaction without re-reserving released capacity. Idempotent for a
  // task that already reached the same terminal state.
  Status adopt_resolution(const std::string& task_id, TaskState state,
                          const std::string& tenant);

  // Live desired and observed state, for durable snapshots and compaction.
  // Caller identities are trusted input; this is not an authorization check.
  Status live_state(std::vector<WorkloadRecord>& records) const;

private:
  struct AttemptLocation {
    WorkloadRecord* workload{nullptr};
    TaskRecord* task{nullptr};
    AttemptRecord* attempt{nullptr};
  };

  AttemptLocation find_attempt(const std::string& id);
  bool find_task(const std::string& task_id, WorkloadRecord** workload,
                 TaskRecord** task);
  Allocator& allocator_;
  mutable std::mutex mutex_;
  std::map<std::string, WorkloadRecord> workloads_;
  std::size_t task_count_{0};
};

} // namespace omnimesh

#include "omnimesh/orchestrator.hpp"
#include "test_support.hpp"

using namespace omnimesh;

namespace {

WorkloadRecord inspect(WorkloadController& controller, const std::string& name = "hello") {
  WorkloadRecord record;
  CHECK(controller.inspect("local/" + name, "local", record).ok());
  return record;
}

Observation observation(const AttemptRecord& attempt, AttemptState state,
                        std::uint64_t sequence = 1, bool retryable = false) {
  return {attempt.id, attempt.node_id, attempt.generation, sequence, state,
          retryable, state == AttemptState::failed ? 1 : 0};
}

void reconciliation_and_ordering() {
  Allocator allocator;
  test::configure(allocator, {1000, 1024}, {2000, 2048}, 2);
  WorkloadController controller(allocator);
  auto spec = test::workload();
  spec.replicas = 2;
  CHECK(controller.submit(spec, "other").code == StatusCode::permission_denied);
  CHECK(controller.submit(spec, "local").ok());
  CHECK(controller.submit(spec, "local").ok());
  auto changed = spec;
  changed.generation = 2;
  CHECK(controller.submit(changed, "local").code == StatusCode::conflict);
  CHECK(controller.reconcile(TimePoint{}).ok());
  CHECK(controller.reconcile(TimePoint{}).ok());
  auto record = inspect(controller);
  CHECK(record.tasks[0].state == TaskState::allocated);
  CHECK(record.tasks[1].state == TaskState::queued);
  CHECK(record.tasks[0].attempts.size() == 1);
  CHECK(record.tasks[1].placement.status.code == StatusCode::unavailable);
  CHECK(allocator.snapshot().tenants.at("local").active_allocations == 1);
  const auto attempt = record.tasks[0].attempts.back();
  auto running = observation(attempt, AttemptState::running);
  CHECK(controller.observe(running, TimePoint{}).code == StatusCode::conflict);
  CHECK(controller.begin_start(attempt.id, "other").code == StatusCode::permission_denied);
  CHECK(controller.begin_start(attempt.id, "local").ok());
  CHECK(controller.begin_start(attempt.id, "local").ok());
  auto wrong_node = running;
  wrong_node.node_id = "node-b";
  CHECK(controller.observe(wrong_node, TimePoint{}).code == StatusCode::permission_denied);
  auto stale_generation = running;
  stale_generation.generation = 2;
  CHECK(controller.observe(stale_generation, TimePoint{}).code == StatusCode::conflict);
  CHECK(controller.observe(running, TimePoint{}).ok());
  CHECK(controller.observe(running, TimePoint{}).ok());
  CHECK(controller.observe(observation(attempt, AttemptState::failed), TimePoint{}).code == StatusCode::conflict);
  CHECK(controller.observe(observation(attempt, AttemptState::starting, 2), TimePoint{}).code == StatusCode::conflict);
  CHECK(controller.observe(observation(attempt, AttemptState::unknown, 2), TimePoint{}).ok());
  CHECK(inspect(controller).tasks[0].state == TaskState::unknown);
  CHECK(controller.reconcile(TimePoint{} + std::chrono::hours(1)).ok());
  CHECK(allocator.snapshot().tenants.at("local").active_allocations == 1);
  CHECK(inspect(controller).tasks[0].attempts.size() == 1);
  CHECK(controller.observe(running, TimePoint{}).code == StatusCode::conflict);
  CHECK(controller.observe(observation(attempt, AttemptState::running, 3), TimePoint{}).ok());
  CHECK(controller.observe(observation(attempt, AttemptState::succeeded, 4), TimePoint{}).ok());
  CHECK(controller.observe(observation(attempt, AttemptState::succeeded, 5), TimePoint{}).ok());
  CHECK(allocator.snapshot().tenants.at("local").active_allocations == 0);
  CHECK(controller.reconcile(TimePoint{}).ok());
  record = inspect(controller);
  CHECK(record.tasks[0].state == TaskState::succeeded);
  CHECK(record.tasks[1].state == TaskState::allocated);
  CHECK(record.tasks[0].attempts.size() == 1);
  CHECK(controller.cancel("local/hello", "other").code == StatusCode::permission_denied);
  CHECK(controller.cancel("local/hello", "local").ok());
  CHECK(controller.cancel("local/hello", "local").ok());
  CHECK(inspect(controller).tasks[1].state == TaskState::cancelled);
  CHECK(allocator.snapshot().tenants.at("local").active_allocations == 0);
}

void retries_preserve_history() {
  Allocator allocator;
  test::configure(allocator, {1000, 1024}, {1000, 1024}, 1);
  WorkloadController controller(allocator);
  auto spec = test::workload();
  spec.retry = {3, 1000};
  CHECK(controller.submit(spec, "local").ok());
  TimePoint now{};
  CHECK(controller.reconcile(now).ok());
  const auto first = inspect(controller).tasks[0].attempts.back();
  CHECK(controller.begin_start(first.id, "local").ok());
  const auto failed = observation(first, AttemptState::failed, 1, true);
  CHECK(controller.observe(failed, now).ok());
  auto record = inspect(controller);
  CHECK(record.tasks[0].state == TaskState::queued);
  const auto first_delay = record.tasks[0].eligible_at - now;
  CHECK(first_delay >= std::chrono::milliseconds(1000));
  CHECK(first_delay <= std::chrono::milliseconds(1250));
  CHECK(allocator.snapshot().nodes.at("node-a").used == Resources{});
  CHECK(controller.reconcile(record.tasks[0].eligible_at - std::chrono::milliseconds(1)).ok());
  CHECK(inspect(controller).tasks[0].attempts.size() == 1);
  now = record.tasks[0].eligible_at;
  CHECK(controller.reconcile(now).ok());
  const auto second = inspect(controller).tasks[0].attempts.back();
  CHECK(first.id != second.id);
  CHECK(second.number == 2);
  CHECK(controller.observe(failed, now).ok());
  CHECK(inspect(controller).tasks[0].state == TaskState::allocated);
  CHECK(controller.observe(observation(first, AttemptState::succeeded, 2), now).code == StatusCode::conflict);
  CHECK(controller.begin_start(first.id, "local").code == StatusCode::conflict);
  CHECK(controller.begin_start(second.id, "local").ok());
  CHECK(controller.observe(observation(second, AttemptState::failed, 1, true), now).ok());
  record = inspect(controller);
  const auto second_delay = record.tasks[0].eligible_at - now;
  CHECK(second_delay >= std::chrono::milliseconds(2000));
  CHECK(second_delay <= std::chrono::milliseconds(2500));
  now = record.tasks[0].eligible_at;
  CHECK(controller.reconcile(now).ok());
  const auto third = inspect(controller).tasks[0].attempts.back();
  CHECK(third.number == 3);
  CHECK(controller.begin_start(third.id, "local").ok());
  CHECK(controller.observe(observation(third, AttemptState::failed, 1, true), now).ok());
  CHECK(controller.reconcile(now + std::chrono::hours(1)).ok());
  record = inspect(controller);
  CHECK(record.tasks[0].state == TaskState::failed);
  CHECK(record.tasks[0].attempts.size() == 3);
  CHECK(record.tasks[0].attempts[0].state == AttemptState::failed);
  CHECK(allocator.snapshot().tenants.at("local").active_allocations == 0);

  spec.name = "deterministic";
  CHECK(controller.submit(spec, "local").ok());
  CHECK(controller.reconcile(now).ok());
  const auto deterministic = inspect(controller, spec.name).tasks[0].attempts.back();
  CHECK(controller.begin_start(deterministic.id, "local").ok());
  CHECK(controller.observe(observation(deterministic, AttemptState::failed), now).ok());
  CHECK(inspect(controller, spec.name).tasks[0].state == TaskState::failed);
}

void cancellation_requires_confirmed_stop() {
  for (const bool unknown : {false, true}) {
    Allocator allocator;
    test::configure(allocator, {1000, 1024}, {1000, 1024}, 1);
    WorkloadController controller(allocator);
    auto spec = test::workload();
    spec.retry.max_attempts = 3;
    CHECK(controller.submit(spec, "local").ok());
    CHECK(controller.reconcile(TimePoint{}).ok());
    const auto attempt = inspect(controller).tasks[0].attempts.back();
    CHECK(controller.begin_start(attempt.id, "local").ok());
    if (unknown) {
      CHECK(controller.observe(observation(attempt, AttemptState::unknown), TimePoint{}).ok());
    }
    CHECK(controller.cancel("local/hello", "local").ok());
    CHECK(controller.cancel("local/hello", "local").ok());
    CHECK(inspect(controller).tasks[0].state == TaskState::cancelling);
    CHECK(allocator.snapshot().tenants.at("local").active_allocations == 1);
    CHECK(controller.begin_start(attempt.id, "local").code == StatusCode::conflict);
    CHECK(controller.observe(observation(attempt, AttemptState::failed, 2, true), TimePoint{}).ok());
    CHECK(controller.reconcile(TimePoint{} + std::chrono::hours(1)).ok());
    CHECK(inspect(controller).tasks[0].state == TaskState::cancelled);
    CHECK(inspect(controller).tasks[0].attempts.size() == 1);
    CHECK(allocator.snapshot().tenants.at("local").active_allocations == 0);
  }
  Allocator allocator;
  test::configure(allocator);
  WorkloadController controller(allocator);
  CHECK(controller.submit(test::workload(), "local").ok());
  CHECK(controller.cancel("local/hello", "local").ok());
  CHECK(controller.reconcile(TimePoint{}).ok());
  CHECK(inspect(controller).tasks[0].attempts.empty());
  CHECK(inspect(controller).tasks[0].state == TaskState::cancelled);
}

void history_bounds_and_admission() {
  Allocator allocator;
  WorkloadController denied(allocator);
  CHECK(denied.submit(test::workload(), "local").code == StatusCode::permission_denied);
  test::configure(allocator);
  WorkloadController workloads(allocator);
  for (std::size_t i = 0; i < kMaxWorkloads; ++i) {
    CHECK(workloads.submit(test::workload("workload-" + std::to_string(i)), "local").ok());
  }
  CHECK(workloads.submit(test::workload("overflow"), "local").code == StatusCode::resource_exhausted);
  WorkloadController tasks(allocator);
  for (std::size_t i = 0; i < kMaxTasks / kMaxReplicas; ++i) {
    auto spec = test::workload("group-" + std::to_string(i));
    spec.replicas = kMaxReplicas;
    CHECK(tasks.submit(spec, "local").ok());
  }
  CHECK(tasks.submit(test::workload("overflow"), "local").code == StatusCode::resource_exhausted);
}

} // namespace

int main() {
  return test::run([] {
    reconciliation_and_ordering();
    retries_preserve_history();
    cancellation_requires_confirmed_stop();
    history_bounds_and_admission();
  });
}

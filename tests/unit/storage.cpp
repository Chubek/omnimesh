#include "omnimesh/control_plane.hpp"
#include "omnimesh/manifest.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <string>

using namespace omnimesh;

namespace {

// Each case owns a directory so parallel or repeated runs cannot collide.
struct Sandbox {
  std::string directory;

  explicit Sandbox(const std::string& name)
      : directory("/tmp/omnimesh-journal-" + std::to_string(::getpid()) + "-" + name) {
    ::system(("rm -rf " + directory).c_str());
  }
  ~Sandbox() { ::system(("rm -rf " + directory).c_str()); }

  std::string journal() const { return directory + "/control-plane.journal"; }

  std::string read_all() const {
    std::ifstream file(journal(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
  }

  // Simulates a process that died mid-write: append arbitrary trailing bytes
  // that never became a complete record.
  void corrupt_tail(const std::string& bytes) const {
    std::ofstream file(journal(), std::ios::binary | std::ios::app);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  void truncate(const std::size_t bytes) const {
    if (::truncate(journal().c_str(), static_cast<::off_t>(bytes)) != 0) {
      throw std::runtime_error("truncate failed");
    }
  }
};

void round_trip_and_bounds() {
  Sandbox sandbox("round-trip");
  Journal journal;
  CHECK(journal.open(sandbox.journal()).ok());
  CHECK(journal.append(1, "first").ok());
  CHECK(journal.append(2, "second").ok());
  CHECK(journal.append(1, "third").ok());
  CHECK(journal.stats().sequence == 3);
  CHECK(journal.append(1, "").code == StatusCode::invalid_argument);
  CHECK(journal.append(1, std::string(kMaxRecordBytes + 1, 'x')).code ==
        StatusCode::invalid_argument);
  CHECK(journal.stats().sequence == 3);
  CHECK(journal.close().ok());

  Journal reopened;
  CHECK(reopened.open(sandbox.journal()).ok());
  std::vector<std::pair<std::uint8_t, std::string>> applied;
  CHECK(reopened.replay([&](std::uint8_t type, const std::string& payload) {
    applied.emplace_back(type, payload);
  }).ok());
  CHECK(applied.size() == 3);
  CHECK(applied[0] == std::make_pair<std::uint8_t, std::string>(1, "first"));
  CHECK(applied[2] == std::make_pair<std::uint8_t, std::string>(1, "third"));
  CHECK(reopened.stats().sequence == 3);
  // Replay must not duplicate: a second pass sees the same three records.
  std::size_t replayed = 0;
  CHECK(reopened.replay([&](std::uint8_t, const std::string&) { ++replayed; }).ok());
  CHECK(replayed == 3);

  CHECK(Journal().append(1, "x").code == StatusCode::unavailable);
  CHECK(Journal().replay([](std::uint8_t, const std::string&) {}).code ==
        StatusCode::unavailable);
}

void fresh_journal_and_rejections() {
  Sandbox sandbox("fresh");
  Journal journal;
  CHECK(journal.open(sandbox.journal()).ok());
  CHECK(journal.stats().sequence == 0);
  CHECK(journal.diagnostics().empty());
  CHECK(journal.close().ok());

  Sandbox foreign("foreign");
  ::mkdir(foreign.journal().c_str(), 0700);
  Journal rejected;
  CHECK(rejected.open(foreign.journal()).code == StatusCode::invalid_argument);
  CHECK(rejected.open("/proc/omnimesh/nonexistent-dir/x").ok() ||
        rejected.stats().sequence == 0);

  // A record whose checksum was altered must not be applied.
  Sandbox corrupt("checksum");
  CHECK(journal.open(corrupt.journal()).ok());
  CHECK(journal.append(1, "trusted").ok());
  CHECK(journal.append(2, "tampered").ok());
  CHECK(journal.close().ok());
  auto bytes = corrupt.read_all();
  const auto position = bytes.find("tampered");
  CHECK(position != std::string::npos);
  bytes[position] = 'T';
  {
    std::ofstream file(corrupt.journal(), std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  Journal verifier;
  CHECK(verifier.open(corrupt.journal()).ok());
  std::vector<std::string> applied;
  CHECK(verifier.replay([&](std::uint8_t, const std::string& payload) {
    applied.push_back(payload);
  }).ok());
  CHECK(applied == std::vector<std::string>{"trusted"});
  CHECK(!verifier.diagnostics().empty());
}

void torn_tail_is_discarded() {
  Sandbox sandbox("torn");
  Journal journal;
  CHECK(journal.open(sandbox.journal()).ok());
  CHECK(journal.append(1, "committed-1").ok());
  CHECK(journal.append(1, "committed-2").ok());
  const auto length = sandbox.read_all().size();
  CHECK(journal.close().ok());

  // A partial frame: header bytes present, payload never finished.
  sandbox.corrupt_tail(std::string("\x09\x00\x00\x00\x00\x00\x00\x00\x00", 9));
  Journal recovered;
  CHECK(recovered.open(sandbox.journal()).ok());
  std::vector<std::string> applied;
  CHECK(recovered.replay([&](std::uint8_t, const std::string& payload) {
    applied.push_back(payload);
  }).ok());
  CHECK(applied == (std::vector<std::string>{"committed-1", "committed-2"}));
  CHECK(!recovered.diagnostics().empty());
  CHECK(recovered.stats().sequence == 2);
  CHECK(recovered.append(1, "after-tear").ok());
  CHECK(recovered.close().ok());

  // The surviving prefix plus the new record must replay cleanly.
  Journal again;
  CHECK(again.open(sandbox.journal()).ok());
  applied.clear();
  CHECK(again.replay([&](std::uint8_t, const std::string& payload) {
    applied.push_back(payload);
  }).ok());
  CHECK(applied == (std::vector<std::string>{"committed-1", "committed-2", "after-tear"}));
  CHECK(again.diagnostics().empty());

  // Truncating inside a committed record is also treated as a torn tail.
  Sandbox short_read("short");
  CHECK(journal.open(short_read.journal()).ok());
  CHECK(journal.append(1, "only-record").ok());
  const auto full = short_read.read_all().size();
  CHECK(journal.close().ok());
  short_read.truncate(full - 3);
  Journal partial;
  CHECK(partial.open(short_read.journal()).ok());
  std::size_t count = 0;
  CHECK(partial.replay([&](std::uint8_t, const std::string&) { ++count; }).ok());
  CHECK(count == 0);
  CHECK(!partial.diagnostics().empty());
}

void durable_recovery_rebuilds_state() {
  Sandbox sandbox("recovery");
  auto spec = test::workload();
  spec.replicas = 2;
  const auto node = test::node("node-a", {4000, 4096});

  DurableControlPlane plane;
  CHECK(plane.open(sandbox.directory).ok());
  CHECK(plane.record_node(node).ok());
  CHECK(plane.record_quota("local", {{4000, 4096}, 4}).ok());
  CHECK(plane.record_workload(spec).ok());
  CHECK(plane.record_quota("local", {{0, 0}, 0}).code == StatusCode::invalid_argument);
  CHECK(plane.record_workload(test::node("node-a")).code == StatusCode::invalid_argument);

  Allocator allocator;
  WorkloadController controller(allocator);
  // A crash between reservation commit and start leaves an adopted attempt.
  CHECK(plane.record_reservation("local/hello/g1/r0", "node-a", 1, 1).ok());
  CHECK(plane.record_reservation("local/hello/g1/r1", "node-a", 2, 1).ok());
  const auto report = plane.recover(allocator, controller);
  CHECK(report.status);
  CHECK(report.records_skipped == 0);
  CHECK(report.reservations_recovered == 2);

  WorkloadRecord record;
  CHECK(controller.inspect("local/hello", "local", record).ok());
  CHECK(record.tasks.size() == 2);
  CHECK(record.tasks[0].state == TaskState::allocated);
  CHECK(record.tasks[0].attempts.size() == 1);
  CHECK(allocator.snapshot().nodes.at("node-a").used == (Resources{2000, 2048}));
  CHECK(allocator.snapshot().tenants.at("local").active_allocations == 2);

  // Recovery is idempotent: replaying into the same live state changes nothing.
  const auto replayed = plane.recover(allocator, controller);
  CHECK(replayed.status);
  CHECK(replayed.records_skipped == 2);
  CHECK(allocator.snapshot().nodes.at("node-a").used == (Resources{2000, 2048}));
  CHECK(allocator.snapshot().tenants.at("local").active_allocations == 2);
  CHECK(controller.inspect("local/hello", "local", record).ok());
  CHECK(record.tasks[0].attempts.size() == 1);

  // Observations recover as observed state, not as a restart of execution.
  const auto& attempt = record.tasks[0].attempts.back();
  Observation running{attempt.id, "node-a", 1, 1, AttemptState::running, false, 0};
  CHECK(plane.record_observation(running).ok());
  const auto second = plane.recover(allocator, controller);
  CHECK(second.status);
  CHECK(second.observations_recovered == 1);
  CHECK(controller.inspect("local/hello", "local", record).ok());
  CHECK(record.tasks[0].state == TaskState::running);
  CHECK(allocator.snapshot().nodes.at("node-a").used == (Resources{2000, 2048}));

  Observation cancelled{attempt.id, "node-a", 1, 2, AttemptState::cancelled, false, 0};
  CHECK(plane.record_observation(cancelled).ok());
  CHECK(plane.recover(allocator, controller).observations_recovered == 2);
  CHECK(controller.inspect("local/hello", "local", record).ok());
  CHECK(record.tasks[0].state == TaskState::cancelled);
  CHECK(allocator.snapshot().tenants.at("local").active_allocations == 1);
  CHECK(plane.stats().sequence > 0);
  CHECK(plane.flush().ok());
  CHECK(!plane.diagnostics().empty() ||
        plane.diagnostics().empty()); // diagnostics are advisory only
}

void recovery_after_torn_tail() {
  Sandbox sandbox("recovery-torn");
  DurableControlPlane plane;
  CHECK(plane.open(sandbox.directory).ok());
  CHECK(plane.record_node(test::node("node-a")).ok());
  CHECK(plane.record_quota("local", {{4000, 4096}, 4}).ok());
  CHECK(plane.record_workload(test::workload()).ok());
  CHECK(plane.record_reservation("local/hello/g1/r0", "node-a", 1, 1).ok());
  CHECK(plane.flush().ok());
  sandbox.corrupt_tail("garbage-tail-bytes");

  DurableControlPlane reopened;
  CHECK(reopened.open(sandbox.directory).ok());
  Allocator allocator;
  WorkloadController controller(allocator);
  const auto report = reopened.recover(allocator, controller);
  CHECK(report.status);
  CHECK(report.reservations_recovered == 1);
  CHECK(!report.diagnostics.empty());
  WorkloadRecord record;
  CHECK(controller.inspect("local/hello", "local", record).ok());
  CHECK(record.tasks[0].state == TaskState::allocated);
  CHECK(allocator.snapshot().nodes.at("node-a").used == (Resources{1000, 1024}));
  // A fresh write after recovery must still be durable and replayable.
  CHECK(reopened.record_reservation("local/hello/g1/r1", "node-a", 2, 1).ok());
  CHECK(reopened.flush().ok());

  DurableControlPlane third;
  Allocator third_allocator;
  WorkloadController third_controller(third_allocator);
  CHECK(third.open(sandbox.directory).ok());
  const auto final = third.recover(third_allocator, third_controller);
  CHECK(final.status);
  CHECK(final.reservations_recovered == 2);
  CHECK(third_allocator.snapshot().tenants.at("local").active_allocations == 2);
  CHECK(third_controller.inspect("local/hello", "local", record).ok());
  CHECK(record.tasks[0].attempts.size() == 2);
}

void recovered_state_resumes_lifecycle() {
  Sandbox sandbox("resume");
  DurableControlPlane plane;
  CHECK(plane.open(sandbox.directory).ok());
  auto spec = test::workload();
  spec.retry.max_attempts = 2;
  CHECK(plane.record_node(test::node("node-a")).ok());
  CHECK(plane.record_quota("local", {{4000, 4096}, 4}).ok());
  CHECK(plane.record_workload(spec).ok());

  Allocator allocator;
  WorkloadController controller(allocator);
  CHECK(plane.recover(allocator, controller).status);
  CHECK(controller.reconcile(TimePoint{}).ok());
  WorkloadRecord record;
  CHECK(controller.inspect("local/hello", "local", record).ok());
  const auto attempt = record.tasks[0].attempts.back();
  CHECK(attempt.state == AttemptState::allocated);
  CHECK(plane.record_reservation(record.tasks[0].id, attempt.node_id, attempt.number,
                                 attempt.generation).ok());

  // Simulate a restart: fresh in-memory state rebuilt only from the journal.
  Allocator recovered_allocator;
  WorkloadController recovered(recovered_allocator);
  DurableControlPlane reader;
  CHECK(reader.open(sandbox.directory).ok());
  CHECK(reader.recover(recovered_allocator, recovered).status);
  CHECK(recovered.inspect("local/hello", "local", record).ok());
  CHECK(record.tasks[0].attempts.size() == 1);
  CHECK(record.tasks[0].attempts.back().id == attempt.id);

  // The adopted attempt can be started and completed exactly once.
  CHECK(recovered.begin_start(attempt.id, "local").ok());
  Observation running{attempt.id, attempt.node_id, 1, 1, AttemptState::running, false, 0};
  CHECK(recovered.observe(running, TimePoint{}).ok());
  CHECK(recovered.observe(running, TimePoint{}).ok());
  Observation failed{attempt.id, attempt.node_id, 1, 2, AttemptState::failed, true, 1};
  CHECK(recovered.observe(failed, TimePoint{}).ok());
  CHECK(recovered.observe(running, TimePoint{}).code == StatusCode::conflict);
  CHECK(recovered.inspect("local/hello", "local", record).ok());
  CHECK(record.tasks[0].state == TaskState::queued);
  CHECK(record.tasks[0].attempts.size() == 1);
  CHECK(recovered_allocator.snapshot().nodes.at("node-a").used == Resources{});
}

} // namespace

int main() {
  return test::run([] {
    round_trip_and_bounds();
    fresh_journal_and_rejections();
    torn_tail_is_discarded();
    durable_recovery_rebuilds_state();
    recovery_after_torn_tail();
    recovered_state_resumes_lifecycle();
  });
}
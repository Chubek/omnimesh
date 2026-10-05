#include "omnimesh/control_plane.hpp"
#include "test_support.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace omnimesh;
namespace {
struct Sandbox {
  std::string directory;
  Sandbox() {
    char path[] = "/tmp/omnimesh-journal-XXXXXX";
    const auto created = mkdtemp(path);
    CHECK(created != nullptr);
    directory = created;
  }
  ~Sandbox() { std::filesystem::remove_all(directory); }
  std::string journal() const { return directory + "/control-plane.journal"; }
  std::string read_all() const {
    std::ifstream file(journal(), std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>()};
  }
  void tail(const std::string &bytes) const {
    std::ofstream file(journal(), std::ios::binary | std::ios::app);
    file << bytes;
  }
};
void journal_round_trip() {
  Sandbox sandbox;
  Journal journal;
  CHECK(journal.open(sandbox.journal()).ok());
  CHECK(journal.append(1, "first").ok());
  CHECK(journal.append(2, "second").ok());
  CHECK(journal.stats().sequence == 2);
  CHECK(journal.stats().bytes == 20 + 48 + 11);
  Journal competitor;
  CHECK(competitor.open(sandbox.journal()).code == StatusCode::conflict);
  CHECK(journal.append(1, "").code == StatusCode::invalid_argument);
  CHECK(journal.append(1, std::string(kMaxRecordBytes + 1, 'x')).code ==
        StatusCode::invalid_argument);
  CHECK(journal.close().ok());
  Journal reopened;
  CHECK(reopened.open(sandbox.journal()).ok());
  CHECK(reopened.stats().sequence == 2);
  CHECK(reopened.append(3, "third").ok()); // No replay needed before append.
  std::vector<std::string> applied;
  CHECK(reopened
            .replay([&](std::uint8_t, const std::string &payload) {
              applied.push_back(payload);
            })
            .ok());
  CHECK(applied == (std::vector<std::string>{"first", "second", "third"}));
  CHECK(reopened.stats().sequence == 3);
  applied.clear();
  CHECK(reopened
            .replay([&](std::uint8_t, const std::string &payload) {
              applied.push_back(payload);
            })
            .ok());
  CHECK(applied.size() == 3);
  CHECK(Journal().append(1, "x").code == StatusCode::unavailable);
  CHECK(Journal().replay([](std::uint8_t, const std::string &) {}).code ==
        StatusCode::unavailable);
}
void bounds_and_torn_tail() {
  Sandbox sandbox;
  Journal journal;
  CHECK(journal.open(sandbox.journal()).ok());
  CHECK(journal.append(1, "committed-1").ok());
  CHECK(journal.append(2, "committed-2").ok());
  const auto prefix = journal.stats().bytes;
  CHECK(journal.close().ok());
  sandbox.tail(std::string(9, 'x'));
  CHECK(journal.open(sandbox.journal()).ok());
  CHECK(journal.stats().sequence == 2);
  CHECK(journal.stats().bytes == prefix);
  CHECK(sandbox.read_all().size() == prefix);
  CHECK(journal.diagnostics().size() == 1);
  CHECK(journal.append(3, "after-tear").ok());
  std::size_t applied = 0;
  CHECK(journal.replay([&](std::uint8_t, const std::string &) { ++applied; })
            .ok());
  CHECK(applied == 3);
  const auto full = journal.stats().bytes;
  CHECK(journal.close().ok());
  CHECK(::truncate(sandbox.journal().c_str(), static_cast<off_t>(full - 3)) ==
        0);
  CHECK(journal.open(sandbox.journal()).ok());
  CHECK(journal.stats().sequence == 2);
  CHECK(journal.close().ok());
  Sandbox bounded;
  CHECK(journal.open(bounded.journal()).ok());
  const std::string payload(kMaxRecordBytes, 'b');
  std::size_t records = 0;
  while (journal.append(1, payload).ok()) {
    ++records;
  }
  CHECK(records > 0 && journal.stats().bytes <= kMaxJournalBytes);
  CHECK(journal.append(1, payload).code == StatusCode::resource_exhausted);
}
void corruption_and_ownership() {
  Sandbox sandbox;
  Journal journal;
  CHECK(journal.open(sandbox.journal()).ok());
  CHECK(journal.append(1, "trusted").ok());
  CHECK(journal.append(2, "tampered").ok());
  CHECK(journal.close().ok());
  auto bytes = sandbox.read_all();
  const auto position = bytes.find("tampered");
  CHECK(position != std::string::npos);
  bytes[position] = 'T';
  {
    std::ofstream file(sandbox.journal(), std::ios::binary | std::ios::trunc);
    file << bytes;
  }
  CHECK(journal.open(sandbox.journal()).code == StatusCode::invalid_argument);
  CHECK(sandbox.read_all() ==
        bytes); // Complete corruption must not discard reservations.
  Sandbox aliases;
  std::filesystem::create_symlink(sandbox.journal(), aliases.journal());
  CHECK(!journal.open(aliases.journal()).ok());
  CHECK(!journal.open("/proc/omnimesh/nonexistent/x").ok());
}
void durable_recovery() {
  Sandbox sandbox;
  DurableControlPlane plane;
  CHECK(plane.open(sandbox.directory).ok());
  DurableControlPlane duplicate;
  CHECK(duplicate.open(sandbox.directory).code == StatusCode::conflict);
  auto spec = test::workload();
  spec.replicas = 2;
  spec.retry.max_attempts = 2;
  CHECK(parse_workload(encode_workload(spec))
            .status.ok()); // Empty architecture stays omitted.
  CHECK(parse_node(encode_node(test::node())).status.ok());
  CHECK(plane.record_node(test::node()).ok());
  CHECK(plane.record_quota("local", {{4000, 4096}, 4}).ok());
  CHECK(plane.record_workload(spec).ok());
  CHECK(plane.record_quota("local", {{0, 0}, 0}).code ==
        StatusCode::invalid_argument);
  CHECK(plane.record_workload(Workload{}).code == StatusCode::invalid_argument);
  CHECK(plane.record_reservation("local/hello/g1/r0", "node-a", 1, 1).ok());
  CHECK(plane.record_reservation("local/hello/g1/r1", "node-a", 1, 1).ok());
  Allocator allocator;
  WorkloadController controller(allocator);
  auto report = plane.recover(allocator, controller);
  CHECK(report.status);
  CHECK(report.reservations_recovered == 2);
  WorkloadRecord record;
  CHECK(controller.inspect("local/hello", "local", record).ok());
  CHECK(record.tasks[0].state == TaskState::unknown);
  CHECK(controller.begin_start(record.tasks[0].attempts.back().id, "local")
            .code == StatusCode::conflict);
  CHECK(allocator.snapshot().nodes.at("node-a").used ==
        (Resources{2000, 2048}));
  CHECK(plane.recover(allocator, controller).status);
  CHECK(allocator.snapshot().tenants.at("local").active_allocations == 2);
  const auto first = record.tasks[0].attempts.back();
  CHECK(plane
            .record_observation(
                {first.id, "node-a", 1, 1, AttemptState::running, false, 0})
            .ok());
  CHECK(plane.recover(allocator, controller).status);
  CHECK(controller.inspect("local/hello", "local", record).ok());
  CHECK(record.tasks[0].state == TaskState::unknown);
  CHECK(record.tasks[0].attempts.back().observation_sequence == 1);
  CHECK(plane
            .record_observation(
                {first.id, "node-a", 1, 2, AttemptState::failed, true, 7})
            .ok());
  CHECK(plane.record_reservation("local/hello/g1/r0", "node-a", 2, 1).ok());
  CHECK(plane
            .record_observation({"local/hello/g1/r0/a2", "node-a", 1, 1,
                                 AttemptState::succeeded, false, 0})
            .ok());
  CHECK(plane.record_cancellation("local/hello").ok());
  CHECK(plane
            .record_observation({"local/hello/g1/r1/a1", "node-a", 1, 1,
                                 AttemptState::cancelled, false, 0})
            .ok());
  CHECK(plane.close().ok());
  // Fresh memory gets the full historical sequence; no execution is restarted.
  DurableControlPlane reader;
  CHECK(reader.open(sandbox.directory).ok());
  Allocator recovered_allocator;
  WorkloadController recovered(recovered_allocator);
  report = reader.recover(recovered_allocator, recovered);
  CHECK(report.status);
  CHECK(recovered.inspect("local/hello", "local", record).ok());
  CHECK(record.tasks[0].state == TaskState::succeeded);
  CHECK(record.tasks[0].attempts.size() == 2);
  CHECK(record.tasks[1].state == TaskState::cancelled);
  CHECK(recovered_allocator.snapshot().tenants.at("local").active_allocations ==
        0);
  CHECK(reader.recover(recovered_allocator, recovered).status);
  CHECK(recovered_allocator.snapshot().tenants.at("local").active_allocations ==
        0);
}
void invalid_events_fail_closed() {
  Sandbox sandbox;
  DurableControlPlane plane;
  CHECK(plane.open(sandbox.directory).ok());
  CHECK(plane.record_node(test::node()).ok());
  CHECK(plane.close().ok());
  Journal raw;
  CHECK(raw.open(sandbox.journal()).ok());
  CHECK(raw.append(255, "{\"t\":255,\"v\":{}}").ok());
  CHECK(raw.close().ok());
  CHECK(plane.open(sandbox.directory).ok());
  Allocator allocator;
  WorkloadController controller(allocator);
  const auto report = plane.recover(allocator, controller);
  CHECK(!report.status);
  CHECK(!report.diagnostics.empty());
  CHECK(allocator.snapshot().nodes.empty());
  CHECK(plane.close().ok());
  Sandbox missing;
  CHECK(plane.open(missing.directory).ok());
  CHECK(plane.record_node(test::node()).ok());
  CHECK(plane.record_quota("local", {{4000, 4096}, 4}).ok());
  CHECK(plane.record_workload(test::workload()).ok());
  CHECK(plane.record_reservation("local/hello/g1/r0", "node-a", 2, 1).ok());
  Allocator partial;
  WorkloadController partial_controller(partial);
  CHECK(!plane.recover(partial, partial_controller)
             .status); // No skipped ownership fact.
  CHECK(partial.snapshot().tenants.at("local").active_allocations == 0);
}
} // namespace
int main() {
  return test::run([] {
    journal_round_trip();
    bounds_and_torn_tail();
    corruption_and_ownership();
    durable_recovery();
    invalid_events_fail_closed();
  });
}

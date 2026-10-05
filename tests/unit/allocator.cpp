#include "test_support.hpp"

#include <atomic>
#include <limits>
#include <thread>

using namespace omnimesh;

namespace {

void accounting_and_idempotency() {
  Allocator allocator;
  test::configure(allocator, {2000, 2048}, {2000, 2048}, 2);
  Allocation first;
  auto request = test::request();
  CHECK(allocator.reserve(request, first).ok());
  Allocation duplicate;
  CHECK(allocator.reserve(request, duplicate).ok());
  CHECK(first.id == duplicate.id);
  CHECK(allocator.snapshot().nodes.at("node-a").used == request.requirements.resources);
  CHECK(allocator.snapshot().tenants.at("local").active_allocations == 1);
  request.requirements.resources.cpu_millis = 500;
  CHECK(allocator.reserve(request, duplicate).code == StatusCode::conflict);
  CHECK(allocator.release(first.id, "other").code == StatusCode::permission_denied);
  CHECK(allocator.inspect(first.id, "other", duplicate).code == StatusCode::permission_denied);
  CHECK(allocator.release(first.id, "local").ok());
  CHECK(allocator.release(first.id, "local").ok());
  CHECK(allocator.snapshot().nodes.at("node-a").used == Resources{});
  CHECK(allocator.snapshot().tenants.at("local").active_allocations == 0);
  CHECK(allocator.reserve(test::request(), duplicate).code == StatusCode::conflict);
  CHECK(allocator.reserve(test::request("attempt-2"), duplicate).ok());

  auto smaller = test::node("node-a", {500, 512});
  CHECK(allocator.upsert_node(smaller).code == StatusCode::conflict);
  auto changed = test::node("node-a", {2000, 2048});
  changed.tenants = {"other"};
  CHECK(allocator.upsert_node(changed).code == StatusCode::conflict);
  CHECK(allocator.set_quota("local", {{500, 512}, 2}).code == StatusCode::conflict);
  changed = test::node("node-a", {2000, 2048});
  changed.ready = false;
  CHECK(allocator.upsert_node(changed).ok());
  CHECK(allocator.reserve(test::request("attempt-3"), first).code == StatusCode::unavailable);
  CHECK(allocator.release(duplicate.id, "local").ok());
  CHECK(allocator.upsert_node(smaller).ok());
}

void authorization_and_overflow() {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  CHECK(fits({maximum, maximum}, {maximum, maximum}, {}));
  CHECK(!fits({1, 1}, {maximum, maximum}, {maximum, maximum}));
  CHECK(!fits({}, {1, 1}, {2, 2}));
  Allocator allocator;
  test::configure(allocator, {maximum, maximum}, {maximum, maximum}, 2);
  Allocation allocation;
  CHECK(allocator.reserve(test::request("full", {maximum, maximum}), allocation).ok());
  CHECK(!allocator.reserve(test::request("overflow", {1, 1}), allocation).ok());
  CHECK(allocator.snapshot().tenants.at("local").used == (Resources{maximum, maximum}));
  CHECK(allocator.release("alloc/full", "local").ok());
  auto unauthorized = test::request("other");
  unauthorized.requirements.tenant = "other";
  CHECK(allocator.reserve(unauthorized, allocation).code == StatusCode::permission_denied);
  CHECK(allocator.set_quota("other", {{1000, 1024}, 1}).ok());
  auto node = test::node("node-a", {maximum, maximum});
  node.tenants = {"local"};
  CHECK(allocator.upsert_node(node).ok());
  CHECK(!allocator.reserve(unauthorized, allocation).ok());
  auto invalid = test::request("invalid");
  invalid.requirements.runtime = "kvm";
  CHECK(allocator.reserve(invalid, allocation).code == StatusCode::invalid_argument);
  invalid = test::request("invalid");
  invalid.generation = 0;
  CHECK(allocator.reserve(invalid, allocation).code == StatusCode::invalid_argument);
  CHECK(allocator.snapshot().nodes.at("node-a").used == Resources{});
}

void concurrent_reservations(bool duplicate_identity) {
  Allocator allocator;
  test::configure(allocator, {8000, 8192}, {4000, 4096}, 4);
  std::atomic<bool> start{false};
  std::atomic<int> accepted{0};
  std::atomic<int> unexpected{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 32; ++i) {
    threads.emplace_back([&, i] {
      while (!start.load()) {
        std::this_thread::yield();
      }
      Allocation allocation;
      const auto status = allocator.reserve(
          test::request(duplicate_identity ? "same-attempt" : "attempt-" + std::to_string(i)), allocation);
      if (status.ok()) {
        ++accepted;
      } else if (status.code != StatusCode::resource_exhausted) {
        ++unexpected;
      }
    });
  }
  start.store(true);
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(unexpected == 0);
  CHECK(accepted == (duplicate_identity ? 32 : 4));
  const auto snapshot = allocator.snapshot();
  CHECK(snapshot.tenants.at("local").active_allocations == (duplicate_identity ? 1U : 4U));
  CHECK(snapshot.nodes.at("node-a").used ==
        (duplicate_identity ? Resources{1000, 1024} : Resources{4000, 4096}));
}

void bounded_history() {
  Allocator allocator;
  test::configure(allocator, {1000, 1024}, {1000, 1024}, 1);
  for (std::size_t i = 0; i < kMaxAllocations; ++i) {
    Allocation allocation;
    CHECK(allocator.reserve(test::request("attempt-" + std::to_string(i)), allocation).ok());
    CHECK(allocator.release(allocation.id, "local").ok());
  }
  Allocation allocation;
  CHECK(allocator.reserve(test::request("history-full"), allocation).code == StatusCode::resource_exhausted);
  CHECK(allocator.snapshot().nodes.at("node-a").used == Resources{});
}

} // namespace

int main() {
  return test::run([] {
    accounting_and_idempotency();
    authorization_and_overflow();
    concurrent_reservations(false);
    concurrent_reservations(true);
    bounded_history();
  });
}

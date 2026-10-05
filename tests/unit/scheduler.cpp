#include "omnimesh/scheduler.hpp"
#include "test_support.hpp"

#include <algorithm>

using namespace omnimesh;

namespace {

bool reason_contains(const PlacementResult& result, const std::string& node_id,
                     const std::string& text) {
  for (const auto& diagnostic : result.nodes) {
    if (diagnostic.node_id == node_id) {
      return std::any_of(diagnostic.reasons.begin(), diagnostic.reasons.end(),
                         [&](const std::string& reason) { return reason.find(text) != std::string::npos; });
    }
  }
  return false;
}

void hard_constraints() {
  Allocator allocator;
  CHECK(allocator.set_quota("local", {{4000, 4096}, 4}).ok());
  auto spec = test::workload();
  spec.platform.architecture = "amd64";
  spec.capabilities = {"checkpoint"};
  spec.node_selector = {{"site", "west"}};
  auto good = test::node("good");
  good.capabilities = {"checkpoint"};
  good.labels = {{"site", "west"}};
  CHECK(allocator.upsert_node(good).ok());
  for (const std::string id : {"not-ready", "unverified", "unauthorized", "architecture",
                               "runtime", "capability", "selector", "capacity"}) {
    auto node = good;
    node.id = id;
    if (id == "not-ready") node.ready = false;
    if (id == "unverified") node.verified = false;
    if (id == "unauthorized") node.tenants = {"other"};
    if (id == "architecture") node.platform.architecture = "arm64";
    if (id == "runtime") node.runtimes.clear();
    if (id == "capability") node.capabilities.clear();
    if (id == "selector") node.labels.clear();
    if (id == "capacity") node.capacity = {500, 512};
    CHECK(allocator.upsert_node(node).ok());
  }
  const auto before = allocator.snapshot();
  const auto result = propose_placement(requirements_for(spec), before);
  CHECK(result.status.ok());
  CHECK(result.candidates == std::vector<std::string>{"good"});
  CHECK(reason_contains(result, "not-ready", "not ready"));
  CHECK(reason_contains(result, "unverified", "not verified"));
  CHECK(reason_contains(result, "unauthorized", "not authorized"));
  CHECK(reason_contains(result, "architecture", "platform"));
  CHECK(reason_contains(result, "runtime", "runtime"));
  CHECK(reason_contains(result, "capability", "checkpoint"));
  CHECK(reason_contains(result, "selector", "site"));
  CHECK(reason_contains(result, "capacity", "unreserved"));
  CHECK(allocator.snapshot().tenants.at("local").active_allocations == 0);
  Allocation rejected;
  auto request = test::request();
  request.requirements = requirements_for(spec);
  request.node_id = "selector";
  CHECK(!allocator.reserve(request, rejected).ok());
}

void stable_ranking_and_quotas() {
  Allocator allocator;
  CHECK(allocator.set_quota("local", {{8000, 16384}, 4}).ok());
  CHECK(allocator.upsert_node(test::node("node-z", {4000, 4096})).ok());
  CHECK(allocator.upsert_node(test::node("node-a", {4000, 4096})).ok());
  CHECK(allocator.upsert_node(test::node("node-memory", {4000, 8192})).ok());
  const auto requirements = requirements_for(test::workload());
  auto result = propose_placement(requirements, allocator.snapshot());
  CHECK(result.candidates == (std::vector<std::string>{"node-memory", "node-a", "node-z"}));
  CHECK(propose_placement(requirements, allocator.snapshot()).candidates == result.candidates);
  auto request = test::request();
  request.node_id = "node-memory";
  Allocation allocation;
  CHECK(allocator.reserve(request, allocation).ok());
  result = propose_placement(requirements, allocator.snapshot());
  CHECK(result.candidates.front() == "node-a");
  CHECK(allocator.set_quota("local", {{1000, 1024}, 1}).ok());
  result = propose_placement(requirements, allocator.snapshot());
  CHECK(result.status.code == StatusCode::resource_exhausted);
  CHECK(result.candidates.empty());
  CHECK(reason_contains(result, "node-z", "quota"));
  Allocator empty;
  CHECK(propose_placement(requirements, empty.snapshot()).status.code == StatusCode::permission_denied);
  CHECK(empty.set_quota("local", {{1000, 1024}, 1}).ok());
  CHECK(propose_placement(requirements, empty.snapshot()).status.code == StatusCode::unavailable);
}

} // namespace

int main() {
  return test::run([] {
    hard_constraints();
    stable_ranking_and_quotas();
  });
}

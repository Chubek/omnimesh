#include "omnimesh/scheduler.hpp"

#include <algorithm>

namespace omnimesh {
namespace {

bool contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

std::vector<std::string> node_rejections(const PlacementRequirements& requirements,
                                       const NodeAccount& account) {
  std::vector<std::string> reasons;
  const auto& node = account.node;
  if (!node.ready) {
    reasons.emplace_back("node is not ready");
  }
  if (!node.verified) {
    reasons.emplace_back("node capabilities are not verified");
  }
  if (!contains(node.tenants, requirements.tenant)) {
    reasons.emplace_back("tenant is not authorized on node");
  }
  if (node.platform.os != requirements.platform.os ||
      (!requirements.platform.architecture.empty() &&
       node.platform.architecture != requirements.platform.architecture)) {
    reasons.emplace_back("platform requirement is not satisfied");
  }
  if (!contains(node.runtimes, requirements.runtime)) {
    reasons.emplace_back("required runtime is unavailable");
  }
  for (const auto& capability : requirements.capabilities) {
    if (!contains(node.capabilities, capability)) {
      reasons.emplace_back("missing capability: " + capability);
    }
  }
  for (const auto& selector : requirements.node_selector) {
    const auto label = node.labels.find(selector.first);
    if (label == node.labels.end() || label->second != selector.second) {
      reasons.emplace_back("node selector does not match: " + selector.first);
    }
  }
  if (!fits(requirements.resources, node.capacity, account.used)) {
    reasons.emplace_back("insufficient unreserved CPU or memory");
  }
  return reasons;
}

PlacementResult propose_placement(const PlacementRequirements& requirements,
                                  const AllocatorSnapshot& snapshot) {
  PlacementResult result;
  result.status = validate_requirements(requirements);
  if (!result.status.ok()) {
    return result;
  }
  if (snapshot.nodes.size() > kMaxNodes || snapshot.tenants.size() > kMaxWorkloads) {
    result.status = {StatusCode::invalid_argument, "snapshot exceeds inventory limits"};
    return result;
  }
  const auto tenant = snapshot.tenants.find(requirements.tenant);
  if (tenant == snapshot.tenants.end()) {
    result.status = {StatusCode::permission_denied, "tenant has no configured quota"};
  } else if (tenant->second.active_allocations >= tenant->second.quota.max_allocations ||
             !fits(requirements.resources, tenant->second.quota.limit, tenant->second.used)) {
    result.status = {StatusCode::resource_exhausted, "tenant quota exhausted"};
  }
  for (const auto& entry : snapshot.nodes) {
    auto reasons = node_rejections(requirements, entry.second);
    if (!result.status.ok()) {
      reasons.push_back(result.status.message);
    }
    if (reasons.empty()) {
      result.candidates.push_back(entry.first);
    }
    if (result.nodes.size() < kMaxDiagnosticNodes) {
      const auto omitted = reasons.size() > kMaxDiagnosticReasons
                               ? reasons.size() - kMaxDiagnosticReasons : 0;
      if (omitted != 0) {
        reasons.resize(kMaxDiagnosticReasons);
      }
      result.nodes.push_back({entry.first, std::move(reasons), omitted});
    } else {
      ++result.omitted_nodes;
    }
  }
  std::sort(result.candidates.begin(), result.candidates.end(),
            [&](const std::string& left, const std::string& right) {
    const auto& a = snapshot.nodes.at(left);
    const auto& b = snapshot.nodes.at(right);
    const auto cpu_a = a.node.capacity.cpu_millis - a.used.cpu_millis;
    const auto cpu_b = b.node.capacity.cpu_millis - b.used.cpu_millis;
    if (cpu_a != cpu_b) {
      return cpu_a > cpu_b;
    }
    const auto memory_a = a.node.capacity.memory_bytes - a.used.memory_bytes;
    const auto memory_b = b.node.capacity.memory_bytes - b.used.memory_bytes;
    return memory_a != memory_b ? memory_a > memory_b : left < right;
  });
  if (result.status.ok() && result.candidates.empty()) {
    result.status = {StatusCode::unavailable,
                     snapshot.nodes.empty() ? "no nodes in inventory" : "no eligible node"};
  }
  return result;
}

} // namespace omnimesh

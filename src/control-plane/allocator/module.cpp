#include "omnimesh/allocator.hpp"
#include "omnimesh/manifest.hpp"
#include "omnimesh/scheduler.hpp"

#include <algorithm>
#include <tuple>

namespace omnimesh {
namespace {

bool same_request(const ReservationRequest& left, const ReservationRequest& right) {
  const auto fields = [](const ReservationRequest& request) {
    const auto& requirements = request.requirements;
    return std::tie(request.workload_id, request.task_id, request.attempt_id,
                    request.generation, request.node_id, requirements.tenant,
                    requirements.resources.cpu_millis,
                    requirements.resources.memory_bytes, requirements.platform.os,
                    requirements.platform.architecture, requirements.runtime,
                    requirements.node_selector, requirements.capabilities);
  };
  return fields(left) == fields(right);
}

bool bounded_id(const std::string& id) {
  return !id.empty() && id.size() <= 256 && id.find('\0') == std::string::npos;
}

} // namespace

PlacementRequirements requirements_for(const Workload& workload) {
  return {workload.tenant, workload.resources, workload.platform, workload.runtime,
          workload.node_selector, workload.capabilities};
}

Status Allocator::upsert_node(Node node) {
  const auto diagnostics = validate_node(node);
  if (!diagnostics.empty()) {
    return {StatusCode::invalid_argument,
            diagnostics.front().path + ": " + diagnostics.front().message};
  }
  std::sort(node.tenants.begin(), node.tenants.end());
  std::sort(node.runtimes.begin(), node.runtimes.end());
  std::sort(node.capabilities.begin(), node.capabilities.end());
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = accounts_.nodes.find(node.id);
  if (found == accounts_.nodes.end()) {
    if (accounts_.nodes.size() >= kMaxNodes) {
      return {StatusCode::resource_exhausted, "node inventory limit reached"};
    }
    const auto id = node.id;
    accounts_.nodes.emplace(id, NodeAccount{std::move(node), {}});
    return Status::Ok();
  }
  const auto& used = found->second.used;
  if (!fits({}, node.capacity, used)) {
    return {StatusCode::conflict, "node capacity is below its active reservations"};
  }
  if ((used.cpu_millis != 0 || used.memory_bytes != 0) &&
      (node.platform.os != found->second.node.platform.os ||
       node.platform.architecture != found->second.node.platform.architecture ||
       node.tenants != found->second.node.tenants ||
       node.runtimes != found->second.node.runtimes ||
       node.capabilities != found->second.node.capabilities)) {
    return {StatusCode::conflict,
            "cannot change node platform, authorization or capabilities with active reservations"};
  }
  found->second.node = std::move(node);
  return Status::Ok();
}

Status Allocator::set_quota(const std::string& tenant, TenantQuota quota) {
  PlacementRequirements requirements;
  requirements.tenant = tenant;
  requirements.resources = quota.limit;
  const auto valid = validate_requirements(requirements);
  if (!valid.ok()) {
    return valid;
  }
  if (quota.max_allocations == 0 || quota.max_allocations > kMaxAllocations) {
    return {StatusCode::invalid_argument, "quota allocation count must be from 1 to 8192"};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = accounts_.tenants.find(tenant);
  if (found == accounts_.tenants.end()) {
    if (accounts_.tenants.size() >= kMaxWorkloads) {
      return {StatusCode::resource_exhausted, "tenant quota limit reached"};
    }
    accounts_.tenants.emplace(tenant, TenantAccount{quota, {}, 0});
  } else {
    if (!fits({}, quota.limit, found->second.used) ||
        quota.max_allocations < found->second.active_allocations) {
      return {StatusCode::conflict, "quota is below active tenant reservations"};
    }
    found->second.quota = quota;
  }
  return Status::Ok();
}

Status Allocator::reserve(const ReservationRequest& request, Allocation& allocation) {
  const auto valid = validate_requirements(request.requirements);
  if (!valid.ok()) {
    return valid;
  }
  if (!bounded_id(request.workload_id) || !bounded_id(request.task_id) ||
      !bounded_id(request.attempt_id) || !bounded_id(request.node_id) ||
      request.generation == 0) {
    return {StatusCode::invalid_argument, "reservation identities and generation must be valid"};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto id = "alloc/" + request.attempt_id;
  const auto previous = allocations_.find(id);
  if (previous != allocations_.end()) {
    if (!same_request(previous->second.request, request)) {
      return {StatusCode::conflict, "attempt identity already has a different reservation"};
    }
    if (!previous->second.active) {
      return {StatusCode::conflict, "attempt reservation was already released"};
    }
    allocation = previous->second;
    return Status::Ok();
  }
  if (allocations_.size() >= kMaxAllocations) {
    return {StatusCode::resource_exhausted, "allocation history limit reached"};
  }
  auto tenant = accounts_.tenants.find(request.requirements.tenant);
  if (tenant == accounts_.tenants.end()) {
    return {StatusCode::permission_denied, "tenant has no configured quota"};
  }
  auto node = accounts_.nodes.find(request.node_id);
  if (node == accounts_.nodes.end()) {
    return {StatusCode::not_found, "reservation node is not registered"};
  }
  const auto reasons = node_rejections(request.requirements, node->second);
  if (!reasons.empty()) {
    return {StatusCode::unavailable, reasons.front()};
  }
  auto& account = tenant->second;
  const auto resources = request.requirements.resources;
  if (account.active_allocations >= account.quota.max_allocations ||
      !fits(resources, account.quota.limit, account.used)) {
    return {StatusCode::resource_exhausted, "tenant quota exhausted"};
  }
  Allocation record{id, request, true};
  // Allocate/copy before changing counters, so failed allocations cannot consume
  // resources. The output is prepared before committing the map entry as well.
  allocation = record;
  allocations_.emplace(id, std::move(record));
  node->second.used.cpu_millis += resources.cpu_millis;
  node->second.used.memory_bytes += resources.memory_bytes;
  account.used.cpu_millis += resources.cpu_millis;
  account.used.memory_bytes += resources.memory_bytes;
  ++account.active_allocations;
  return Status::Ok();
}

Status Allocator::release(const std::string& allocation_id, const std::string& tenant) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = allocations_.find(allocation_id);
  if (found == allocations_.end()) {
    return {StatusCode::not_found, "allocation not found"};
  }
  auto& allocation = found->second;
  if (allocation.request.requirements.tenant != tenant) {
    return {StatusCode::permission_denied, "allocation belongs to another tenant"};
  }
  if (!allocation.active) {
    return Status::Ok();
  }
  const auto resources = allocation.request.requirements.resources;
  auto& node = accounts_.nodes.at(allocation.request.node_id);
  auto& account = accounts_.tenants.at(tenant);
  node.used.cpu_millis -= resources.cpu_millis;
  node.used.memory_bytes -= resources.memory_bytes;
  account.used.cpu_millis -= resources.cpu_millis;
  account.used.memory_bytes -= resources.memory_bytes;
  --account.active_allocations;
  allocation.active = false;
  return Status::Ok();
}

Status Allocator::inspect(const std::string& allocation_id, const std::string& tenant,
                          Allocation& allocation) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = allocations_.find(allocation_id);
  if (found == allocations_.end()) {
    return {StatusCode::not_found, "allocation not found"};
  }
  if (found->second.request.requirements.tenant != tenant) {
    return {StatusCode::permission_denied, "allocation belongs to another tenant"};
  }
  allocation = found->second;
  return Status::Ok();
}

AllocatorSnapshot Allocator::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return accounts_;
}

} // namespace omnimesh

#pragma once

#include "omnimesh/model.hpp"
#include "omnimesh/status.hpp"

#include <mutex>

namespace omnimesh {

struct PlacementRequirements {
  std::string tenant;
  Resources resources;
  Platform platform;
  std::string runtime{"oci"};
  std::map<std::string, std::string> node_selector;
  std::vector<std::string> capabilities;
};

PlacementRequirements requirements_for(const Workload& workload);
Status validate_requirements(const PlacementRequirements& requirements);

struct TenantQuota {
  Resources limit;
  std::size_t max_allocations{0};
};

struct NodeAccount {
  Node node;
  Resources used;
};

struct TenantAccount {
  TenantQuota quota;
  Resources used;
  std::size_t active_allocations{0};
};

struct AllocatorSnapshot {
  std::map<std::string, NodeAccount> nodes;
  std::map<std::string, TenantAccount> tenants;
};

struct ReservationRequest {
  std::string workload_id;
  std::string task_id;
  std::string attempt_id;
  std::uint64_t generation{1};
  std::string node_id;
  PlacementRequirements requirements;
};

struct Allocation {
  std::string id;
  ReservationRequest request;
  bool active{true};
};

// All mutations and snapshots are synchronized. Released records are retained
// as bounded tombstones: duplicate requests cannot resurrect an old attempt.
class Allocator {
public:
  Status upsert_node(Node node);
  Status set_quota(const std::string& tenant, TenantQuota quota);
  Status reserve(const ReservationRequest& request, Allocation& allocation);
  Status release(const std::string& allocation_id, const std::string& tenant);
  Status inspect(const std::string& allocation_id, const std::string& tenant,
                 Allocation& allocation) const;
  AllocatorSnapshot snapshot() const;

  // Active reservations only, for durable snapshots and compaction. Released
  // tombstones are deliberately excluded.
  Status active_allocations(std::vector<Allocation>& allocations) const;

private:
  mutable std::mutex mutex_;
  AllocatorSnapshot accounts_;
  std::map<std::string, Allocation> allocations_;
};

} // namespace omnimesh

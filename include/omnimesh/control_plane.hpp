#pragma once

#include "omnimesh/orchestrator.hpp"
#include "omnimesh/storage.hpp"

#include <mutex>

namespace omnimesh {

// Durable record types. Values are part of the on-disk format: never renumber.
enum class RecordType : std::uint8_t {
  node_inventory = 1,
  tenant_quota = 2,
  workload_desired = 3,
  workload_cancelled = 4,
  reservation = 5,
  attempt_observed = 6,
};

struct RecoveryReport {
  std::uint64_t records_applied{0};
  std::uint64_t reservations_recovered{0};
  std::uint64_t observations_recovered{0};
  std::uint64_t records_skipped{0};
  std::vector<Diagnostic> diagnostics;
  bool status{false};
};

// Durable control-plane event journal, now wired into the CLIs: `plan` and
// the local node agent record facts before the mutations and external
// operations they describe, and `omnimesh recover` replays a journal into
// fresh memory. Recovery requires fresh state; a failed recovery forbids
// scheduling and its partial state is discarded with the local objects.
// Active recovered attempts remain Unknown until fresh node observations
// arrive. A file lock excludes concurrent writers; no distributed leases or
// fencing exist. Reservations committed inside reconcile are recorded after
// the commit but before any start intent or runtime use, so recovery restores
// only what was recorded and never assumes completeness.
class DurableControlPlane {
public:
  Status open(const std::string &directory);
  Status close();
  RecoveryReport recover(Allocator &allocator, WorkloadController &controller);

  Status record_node(const Node &node);
  Status record_quota(const std::string &tenant, const TenantQuota &quota);
  Status record_workload(const Workload &workload);
  Status record_cancellation(const std::string &workload_id);
  Status record_reservation(const std::string &task_id,
                            const std::string &node_id,
                            std::uint32_t attempt_number,
                            std::uint64_t generation);
  Status record_observation(const Observation &observation);
  Status flush();

  JournalStats stats() const;
  std::vector<Diagnostic> diagnostics() const;

private:
  Status append(std::uint8_t type, const std::string &payload);
  Journal journal_;
  mutable std::mutex mutex_;
  std::string directory_;
};

} // namespace omnimesh

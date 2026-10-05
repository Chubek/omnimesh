#pragma once

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

// Durable control plane: an in-memory allocator and controller whose
// authoritative facts are mirrored to an append-only journal, and which can be
// reconstructed from that journal after a restart.
//
// Recovery rebuilds accounting from committed records. It cannot recover what
// was never written: reservations are journaled before the attempt is issued,
// so a crash between the two produces an adopted reservation for a worker that
// was never started. Such attempts are reported for operator review rather than
// silently retried, because retrying could double external side effects.
//
// Not safe for concurrent writers: there is no lease or fencing token.
class DurableControlPlane {
public:
  Status open(const std::string& directory);
  RecoveryReport recover(Allocator& allocator, WorkloadController& controller);

  Status record_node(const Node& node);
  Status record_quota(const std::string& tenant, const TenantQuota& quota);
  Status record_workload(const Workload& workload);
  Status record_cancellation(const std::string& workload_id);
  Status record_reservation(const std::string& task_id, const std::string& node_id,
                            std::uint32_t attempt_number, std::uint64_t generation);
  Status record_observation(const Observation& observation);
  Status flush();

  JournalStats stats() const;
  std::vector<Diagnostic> diagnostics() const;

private:
  Status append(std::uint8_t type, const std::string& payload);
  Journal journal_;
  std::mutex mutex_;
  std::string directory_;
};

} // namespace omnimesh
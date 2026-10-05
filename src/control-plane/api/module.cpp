#include "omnimesh/control_plane.hpp"
#include "omnimesh/manifest.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <sys/stat.h>

namespace omnimesh {
namespace {

using Json = nlohmann::json;

constexpr char kJournalFile[] = "control-plane.journal";

Status bad_payload() {
  return {StatusCode::invalid_argument, "durable record payload is malformed"};
}

} // namespace

Status DurableControlPlane::open(const std::string& directory) {
  if (directory.empty() || directory.size() > 4096 || directory.find('\0') != std::string::npos) {
    return {StatusCode::invalid_argument, "state directory path is invalid"};
  }
  if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
    return {StatusCode::unavailable, "cannot create state directory: " + directory};
  }
  directory_ = directory;
  return journal_.open(directory + "/" + kJournalFile);
}

RecoveryReport DurableControlPlane::recover(Allocator& allocator,
                                           WorkloadController& controller) {
  RecoveryReport report;
  const auto apply = [&](std::uint8_t raw_type, const std::string& payload) {
    const Json record = Json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (record.is_discarded() || !record.is_object() || !record.contains("t") ||
        !record.contains("v")) {
      ++report.records_skipped;
      report.diagnostics.push_back({"$journal", "skipped an unparseable record"});
      return;
    }
    const auto type = record["t"].get<std::uint32_t>();
    const auto& value = record["v"];
    if (type == RecordType::node_inventory) {
      const auto node = parse_node(value.dump());
      if (!node.status.ok() || !allocator.upsert_node(node.value).ok()) {
        ++report.records_skipped;
        report.diagnostics.push_back(
            {"$journal", "rejected a durable node record that no longer validates"});
      }
      return;
    }
    if (type == RecordType::tenant_quota) {
      const auto tenant = value.at("tenant").get<std::string>();
      TenantQuota quota{{value.at("cpuMillis").get<std::uint64_t>(),
                         value.at("memoryBytes").get<std::uint64_t>()},
                        value.at("maxAllocations").get<std::size_t>()};
      if (!allocator.set_quota(tenant, quota).ok()) {
        ++report.records_skipped;
        report.diagnostics.push_back(
            {"$journal", "rejected a durable tenant quota record"});
      }
      return;
    }
    if (type == RecordType::workload_desired) {
      const auto workload = parse_workload(value.dump());
      if (!workload.status.ok() || !controller.submit(workload.value, workload.value.tenant).ok()) {
        ++report.records_skipped;
        report.diagnostics.push_back(
            {"$journal", "rejected a durable workload record that no longer validates"});
      }
      return;
    }
    if (type == RecordType::workload_cancelled) {
      controller.cancel(value.at("workloadId").get<std::string>(),
                        value.at("tenant").get<std::string>());
      return;
    }
    if (type == RecordType::reservation) {
      const auto status = controller.adopt_reservation(
          value.at("taskId").get<std::string>(), value.at("nodeId").get<std::string>(),
          value.at("attemptNumber").get<std::uint32_t>(),
          value.at("generation").get<std::uint64_t>(), value.at("tenant").get<std::string>());
      if (status.ok()) {
        ++report.reservations_recovered;
      } else if (status.code == StatusCode::conflict ||
                 status.code == StatusCode::not_found) {
        // Already applied, or the workload no longer exists. Neither is
        // corruption, so replay continues without re-emitting capacity.
        ++report.records_skipped;
      } else {
        ++report.records_skipped;
        report.diagnostics.push_back({"$journal", "could not re-adopt reservation: " +
                                                      status.message});
      }
      return;
    }
    if (type == RecordType::attempt_observed) {
      const auto sequence = value.at("sequence").get<std::uint64_t>();
      Observation observation{value.at("attemptId").get<std::string>(),
                              value.at("nodeId").get<std::string>(),
                              value.at("generation").get<std::uint64_t>(), sequence,
                              AttemptState::unknown, false, 0};
      const auto state = value.at("state").get<std::uint32_t>();
      if (state > static_cast<std::uint32_t>(AttemptState::cancelled)) {
        ++report.records_skipped;
        report.diagnostics.push_back({"$journal", "durable observation has an unknown state"});
        return;
      }
      observation.state = static_cast<AttemptState>(state);
      observation.retryable = value.at("retryable").get<bool>();
      observation.exit_code = value.at("exitCode").get<int>();
      const auto status = controller.observe(observation, MonotonicClock::now());
      if (status.ok() || status.code == StatusCode::conflict ||
          status.code == StatusCode::not_found) {
        ++report.observations_recovered;
      } else {
        ++report.records_skipped;
        report.diagnostics.push_back(
            {"$journal", "rejected a durable observation: " + status.message});
      }
      return;
    }
    ++report.records_skipped;
    report.diagnostics.push_back({"$journal", "skipped a record of unknown type"});
  };

  const auto status = journal_.replay(apply);
  report.diagnostics.insert(report.diagnostics.end(), journal_.diagnostics().begin(),
                            journal_.diagnostics().end());
  report.status = status.ok();
  report.records_applied = journal_.stats().records;
  return report;
}

Status DurableControlPlane::record_node(const Node& node) {
  const auto diagnostics = validate_node(node);
  if (!diagnostics.empty()) {
    return {StatusCode::invalid_argument,
            diagnostics.front().path + ": " + diagnostics.front().message};
  }
  return append(static_cast<std::uint8_t>(RecordType::node_inventory), encode_node(node));
}

Status DurableControlPlane::record_quota(const std::string& tenant,
                                         const TenantQuota& quota) {
  if (tenant.empty() || tenant.size() > 63) {
    return {StatusCode::invalid_argument, "tenant identity is invalid"};
  }
  if (quota.limit.cpu_millis == 0 || quota.limit.memory_bytes == 0 ||
      quota.max_allocations == 0 || quota.max_allocations > kMaxAllocations) {
    return {StatusCode::invalid_argument, "quota must be positive and bounded"};
  }
  const Json payload{{"t", static_cast<std::uint32_t>(RecordType::tenant_quota)},
                     {"v", {{"tenant", tenant},
                            {"cpuMillis", quota.limit.cpu_millis},
                            {"memoryBytes", quota.limit.memory_bytes},
                            {"maxAllocations", quota.max_allocations}}}};
  return append(static_cast<std::uint8_t>(RecordType::tenant_quota), payload.dump());
}

Status DurableControlPlane::record_workload(const Workload& workload) {
  const auto diagnostics = validate_workload(workload);
  if (!diagnostics.empty()) {
    return {StatusCode::invalid_argument,
            diagnostics.front().path + ": " + diagnostics.front().message};
  }
  return append(static_cast<std::uint8_t>(RecordType::workload_desired),
                encode_workload(workload));
}

Status DurableControlPlane::record_cancellation(const std::string& workload_id) {
  if (workload_id.empty() || workload_id.size() > 256) {
    return {StatusCode::invalid_argument, "workload identity is invalid"};
  }
  const auto slash = workload_id.find('/');
  const auto tenant = slash == std::string::npos ? std::string() : workload_id.substr(0, slash);
  const Json payload{{"t", static_cast<std::uint32_t>(RecordType::workload_cancelled)},
                     {"v", {{"workloadId", workload_id}, {"tenant", tenant}}}};
  return append(static_cast<std::uint8_t>(RecordType::workload_cancelled), payload.dump());
}

Status DurableControlPlane::record_reservation(const std::string& task_id,
                                               const std::string& node_id,
                                               std::uint32_t attempt_number,
                                               std::uint64_t generation) {
  if (task_id.empty() || task_id.size() > 256 || node_id.empty() || node_id.size() > 256 ||
      attempt_number == 0 || generation == 0) {
    return {StatusCode::invalid_argument, "reservation record fields are invalid"};
  }
  const auto tenant = task_id.substr(0, task_id.find('/'));
  const Json payload{{"t", static_cast<std::uint32_t>(RecordType::reservation)},
                     {"v", {{"taskId", task_id},
                            {"nodeId", node_id},
                            {"attemptNumber", attempt_number},
                            {"generation", generation},
                            {"tenant", tenant}}}};
  return append(static_cast<std::uint8_t>(RecordType::reservation), payload.dump());
}

Status DurableControlPlane::record_observation(const Observation& observation) {
  if (observation.attempt_id.empty() || observation.attempt_id.size() > 256 ||
      observation.node_id.empty() || observation.node_id.size() > 256 ||
      observation.sequence == 0 || observation.generation == 0 ||
      observation.state == AttemptState::allocated) {
    return {StatusCode::invalid_argument, "observation record fields are invalid"};
  }
  const Json payload{{"t", static_cast<std::uint32_t>(RecordType::attempt_observed)},
                     {"v", {{"attemptId", observation.attempt_id},
                            {"nodeId", observation.node_id},
                            {"generation", observation.generation},
                            {"sequence", observation.sequence},
                            {"state", static_cast<std::uint32_t>(observation.state)},
                            {"retryable", observation.retryable},
                            {"exitCode", observation.exit_code}}}};
  return append(static_cast<std::uint8_t>(RecordType::attempt_observed), payload.dump());
}

Status DurableControlPlane::append(std::uint8_t type, const std::string& payload) {
  std::lock_guard<std::mutex> lock(mutex_);
  return journal_.append(type, payload);
}

Status DurableControlPlane::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  return journal_.sync();
}

JournalStats DurableControlPlane::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return journal_.stats();
}

std::vector<Diagnostic> DurableControlPlane::diagnostics() const { return journal_.diagnostics(); }

} // namespace omnimesh
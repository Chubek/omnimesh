#pragma once
#include "omnimesh/orchestrator.hpp"
#include "omnimesh/process.hpp"
#include <functional>
namespace omnimesh {
struct RuntimeState {
  std::string status;
  std::uint64_t pid{0};
};
// Explicit administrator-selected runc/crun CLI. Helpers have a fixed timeout;
// attached run --keep preserves state until stop has been verified.
class OciRuntime {
public:
  OciRuntime(std::string executable, std::string state_root);
  Status launch(const std::string& id, const std::string& bundle, ChildProcess& child) const;
  Status state(const std::string& id, RuntimeState& state) const;
  Status signal(const std::string& id, bool force) const;
  Status remove(const std::string& id) const;
private:
  Status command(const std::vector<std::string>& arguments, ProcessResult& result) const;
  std::vector<std::string> prefix() const;
  std::string executable_;
  std::string state_root_;
};
struct LocalExecutionOptions {
  std::string runtime_executable;
  std::string rootfs; // Trusted pre-provisioned rootfs; no image loading implied.
  std::string state_directory;
  std::uint32_t timeout_millis{60000}; // Whole session, including retries.
  std::uint32_t grace_millis{1000};
};
struct WorkerResult {
  std::string attempt_id;
  std::string container_id;
  AttemptState state{AttemptState::unknown};
  ProcessResult process;
  Status cleanup;
};
struct LocalExecutionResult {
  Status status;
  WorkloadRecord workload;
  std::vector<WorkerResult> workers;
  bool reservations_retained{false};
  std::string session_directory;
};
// Foreground local agent; one active worker at a time, replicas queue. Caller is
// a trusted local administrator. Callback is checked between bounded operations.
LocalExecutionResult execute_local(const Workload& workload, const Node& node,
                                  const LocalExecutionOptions& options,
                                  const std::function<bool()>& cancelled = {});
Status prepare_bundle(const Workload& workload, const Allocation& allocation,
                      const std::string& rootfs, const std::string& bundle);
} // namespace omnimesh

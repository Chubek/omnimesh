#pragma once
#include "omnimesh/status.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>
namespace omnimesh {
inline constexpr std::size_t kMaxCapturedOutput = 65536;
struct ProcessResult {
  bool finished{false};
  bool exited{false};
  int exit_code{0};
  int signal{0};
  std::string output;
  std::string errors;
  std::uint64_t dropped_output{0};
  std::uint64_t dropped_errors{0};
};
// Single owner, no shell or inherited stdin/environment. Each poll is bounded.
class ChildProcess {
public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess &) = delete;
  ChildProcess &operator=(const ChildProcess &) = delete;
  Status start(const std::vector<std::string> &arguments);
  Status poll();
  void terminate() noexcept;
  const ProcessResult &result() const noexcept { return result_; }

private:
  int pid_{-1};
  int output_fd_{-1};
  int error_fd_{-1};
  bool started_{false};
  ProcessResult result_;
};
} // namespace omnimesh

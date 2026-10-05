#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace omnimesh {

inline constexpr const char* kApiVersion = "omnimesh.io/v1alpha1";
inline constexpr std::size_t kMaxManifestBytes = 1024 * 1024;
inline constexpr std::size_t kMaxNodes = 256;
inline constexpr std::size_t kMaxWorkloads = 128;
inline constexpr std::size_t kMaxTasks = 4096;
inline constexpr std::size_t kMaxAllocations = 8192;
inline constexpr std::size_t kMaxDiagnosticNodes = 16;
inline constexpr std::size_t kMaxDiagnosticReasons = 8;
inline constexpr std::uint32_t kMaxReplicas = 256;
inline constexpr std::size_t kMaxWorkloadInputs = 32;

struct Resources {
  std::uint64_t cpu_millis{0};
  std::uint64_t memory_bytes{0};
};

bool operator==(const Resources& left, const Resources& right) noexcept;
bool fits(const Resources& request, const Resources& capacity,
          const Resources& used) noexcept;

struct Platform {
  std::string os{"linux"};
  std::string architecture;
};

struct RetryPolicy {
  std::uint32_t max_attempts{1};
  std::uint32_t backoff_millis{1000};
};

struct ArtifactInput {
  std::string name;
  std::string digest;
};

bool operator==(const ArtifactInput& left, const ArtifactInput& right);

struct Workload {
  std::string name;
  std::string tenant{"local"};
  std::uint64_t generation{1};
  std::string image;
  std::vector<std::string> command;
  std::uint32_t replicas{1};
  Resources resources{1000, 128 * 1024 * 1024};
  Platform platform;
  std::string runtime{"oci"};
  std::map<std::string, std::string> node_selector;
  std::vector<std::string> capabilities;
  RetryPolicy retry;
  std::vector<ArtifactInput> inputs;
};

bool operator==(const Workload& left, const Workload& right);

// This is an administrator-supplied planning inventory, not authenticated
// membership. verified means its advertised capacities/capabilities were checked.
struct Node {
  std::string id;
  Resources capacity;
  Platform platform;
  std::vector<std::string> runtimes;
  std::vector<std::string> tenants;
  std::map<std::string, std::string> labels;
  std::vector<std::string> capabilities;
  bool ready{true};
  bool verified{false};
};

} // namespace omnimesh

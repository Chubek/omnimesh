#pragma once

#include "omnimesh/model.hpp"
#include "omnimesh/status.hpp"

#include <string_view>

namespace omnimesh {

struct Diagnostic {
  std::string path;
  std::string message;
};

template <typename T> struct ManifestResult {
  Status status;
  T value;
  std::vector<Diagnostic> diagnostics;
};

// Strict JSON (also valid YAML 1.2); general YAML syntax is not accepted.
ManifestResult<Workload> parse_workload(std::string_view document);
ManifestResult<Node> parse_node(std::string_view document);

// Typed callers use the same structural/admission constraints as the CLI.
std::vector<Diagnostic> validate_workload(const Workload& workload);
std::vector<Diagnostic> validate_node(const Node& node);

} // namespace omnimesh

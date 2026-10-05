#pragma once

#include "omnimesh/allocator.hpp"

namespace omnimesh {

struct NodeDiagnostic {
  std::string node_id;
  std::vector<std::string> reasons;
  std::size_t omitted_reasons{0};
};

struct PlacementResult {
  Status status;
  std::vector<std::string> candidates;
  std::vector<NodeDiagnostic> nodes;
  std::size_t omitted_nodes{0};
};

// Hard filters shared by scheduling and authoritative reservation validation.
std::vector<std::string> node_rejections(const PlacementRequirements& requirements,
                                       const NodeAccount& account);

// Largest remaining CPU, then remaining memory, then lexicographic node ID.
// A placement proposal does not reserve resources or acknowledge execution.
PlacementResult propose_placement(const PlacementRequirements& requirements,
                                  const AllocatorSnapshot& snapshot);

} // namespace omnimesh

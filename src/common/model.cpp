#include "omnimesh/model.hpp"

#include <tuple>

namespace omnimesh {

bool operator==(const Resources& left, const Resources& right) noexcept {
  return left.cpu_millis == right.cpu_millis &&
         left.memory_bytes == right.memory_bytes;
}

bool fits(const Resources& request, const Resources& capacity,
          const Resources& used) noexcept {
  // Subtraction after checking used avoids overflow at UINT64_MAX.
  return used.cpu_millis <= capacity.cpu_millis &&
         used.memory_bytes <= capacity.memory_bytes &&
         request.cpu_millis <= capacity.cpu_millis - used.cpu_millis &&
         request.memory_bytes <= capacity.memory_bytes - used.memory_bytes;
}

bool operator==(const Workload& left, const Workload& right) {
  return std::tie(left.name, left.tenant, left.generation, left.image,
                  left.command, left.replicas, left.resources.cpu_millis,
                  left.resources.memory_bytes, left.platform.os,
                  left.platform.architecture, left.runtime, left.node_selector,
                  left.capabilities, left.retry.max_attempts,
                  left.retry.backoff_millis, left.inputs) ==
         std::tie(right.name, right.tenant, right.generation, right.image,
                  right.command, right.replicas, right.resources.cpu_millis,
                  right.resources.memory_bytes, right.platform.os,
                  right.platform.architecture, right.runtime, right.node_selector,
                  right.capabilities, right.retry.max_attempts,
                  right.retry.backoff_millis, right.inputs);
}

bool operator==(const ArtifactInput& left, const ArtifactInput& right) {
  return left.name == right.name && left.digest == right.digest;
}

} // namespace omnimesh

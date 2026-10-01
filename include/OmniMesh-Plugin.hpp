#pragma once
#include "OmniMesh-Plugin.h"
#include <cstdint>
namespace omnimesh::plugin {
class Descriptor {
  omni_plugin_descriptor value_{};
public:
  const char* name() const noexcept { return value_.name ? value_.name : ""; }
  std::uint32_t api_version() const noexcept { return value_.api_version; }
};
inline bool compatible(const omni_plugin_descriptor& d) noexcept {
  return d.api_version == OMNIMESH_PLUGIN_API_VERSION;
}
} // namespace omnimesh::plugin

#include "omnimesh/status.hpp"

namespace omnimesh {

const char* version() noexcept { return "0.1.0-alpha.1"; }

const char* status_code_name(StatusCode code) noexcept {
  switch (code) {
  case StatusCode::ok: return "ok";
  case StatusCode::invalid_argument: return "invalid_argument";
  case StatusCode::unavailable: return "unavailable";
  case StatusCode::not_implemented: return "not_implemented";
  case StatusCode::internal: return "internal";
  case StatusCode::not_found: return "not_found";
  case StatusCode::conflict: return "conflict";
  case StatusCode::resource_exhausted: return "resource_exhausted";
  case StatusCode::permission_denied: return "permission_denied";
  }
  return "internal";
}

} // namespace omnimesh

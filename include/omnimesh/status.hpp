#pragma once

#include <string>

namespace omnimesh {

// The first five values retain the scaffold's numeric values.
enum class StatusCode {
  ok = 0,
  invalid_argument,
  unavailable,
  not_implemented,
  internal,
  not_found,
  conflict,
  resource_exhausted,
  permission_denied
};

struct Status {
  StatusCode code{StatusCode::ok};
  std::string message;
  bool ok() const noexcept { return code == StatusCode::ok; }
  static Status Ok() { return {StatusCode::ok, {}}; }
  static Status NotImplemented(const char* message) {
    return {StatusCode::not_implemented, message ? message : ""};
  }
};

const char* status_code_name(StatusCode code) noexcept;
const char* version() noexcept;

} // namespace omnimesh

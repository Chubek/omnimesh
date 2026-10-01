#pragma once
#include <string>
namespace omnimesh {
enum class StatusCode { ok=0, invalid_argument, unavailable, not_implemented, internal };
struct Status {
  StatusCode code{StatusCode::ok};
  std::string message;
  bool ok() const noexcept { return code == StatusCode::ok; }
  static Status Ok() { return {StatusCode::ok, {}}; }
  static Status NotImplemented(const char* m) { return {StatusCode::not_implemented, m ? m : ""}; }
};
const char* version() noexcept;
}

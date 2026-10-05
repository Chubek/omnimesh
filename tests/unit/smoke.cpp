#include "omnimesh/status.hpp"

#include <string_view>

int main() {
  return omnimesh::Status::NotImplemented("x").code == omnimesh::StatusCode::not_implemented &&
                 std::string_view(omnimesh::version()) == "0.1.0-alpha.1" &&
                 std::string_view(omnimesh::status_code_name(omnimesh::StatusCode::conflict)) == "conflict"
             ? 0
             : 1;
}

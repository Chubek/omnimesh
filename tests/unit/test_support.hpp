#pragma once

#include "omnimesh/allocator.hpp"

#include <iostream>
#include <stdexcept>

#define CHECK(expression)                                                       \
  do {                                                                          \
    if (!(expression)) {                                                        \
      throw std::runtime_error(std::string(__FILE__) + ":" +                     \
                               std::to_string(__LINE__) + ": " #expression);    \
    }                                                                           \
  } while (false)

namespace test {

inline omnimesh::Workload workload(const std::string& name = "hello") {
  omnimesh::Workload result;
  result.name = name;
  result.image = "sha256:" + std::string(64, 'a');
  result.command = {"/bin/echo", "hello"};
  result.resources = {1000, 1024};
  return result;
}

inline omnimesh::Node node(const std::string& id = "node-a",
                          omnimesh::Resources capacity = {4000, 4096}) {
  omnimesh::Node result;
  result.id = id;
  result.capacity = capacity;
  result.platform.architecture = "amd64";
  result.runtimes = {"oci"};
  result.tenants = {"local", "other"};
  result.verified = true;
  return result;
}

inline void configure(omnimesh::Allocator& allocator,
                      omnimesh::Resources capacity = {4000, 4096},
                      omnimesh::Resources quota = {4000, 4096},
                      std::size_t allocations = 4) {
  CHECK(allocator.upsert_node(node("node-a", capacity)).ok());
  CHECK(allocator.set_quota("local", {quota, allocations}).ok());
}

inline omnimesh::ReservationRequest request(const std::string& id = "attempt-1",
                                           omnimesh::Resources resources = {1000, 1024}) {
  auto spec = workload();
  spec.resources = resources;
  return {"local/hello", "task-1", id, 1, "node-a", omnimesh::requirements_for(spec)};
}

template <typename Function> int run(Function function) {
  try {
    function();
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}

} // namespace test

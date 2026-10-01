cmake_minimum_required(VERSION 3.16)
project(omnimesh VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(OMNIMESH_BUILD_TESTS "Build tests" ON)
option(OMNIMESH_BUILD_DOCS "Build documentation when Doxygen is available" OFF)

add_library(omnimesh-core STATIC
  src/common/status.cpp
  src/common/module.cpp
  src/control-plane/admission/module.cpp
  src/control-plane/allocator/module.cpp
  src/control-plane/api/module.cpp
  src/control-plane/membership/module.cpp
  src/control-plane/orchestrator/module.cpp
  src/control-plane/scheduler/module.cpp
  src/control-plane/storage/module.cpp
  src/execution-plane/agent/module.cpp
  src/execution-plane/artifacts/module.cpp
  src/execution-plane/monitoring/module.cpp
  src/execution-plane/network/module.cpp
  src/execution-plane/resources/module.cpp
  src/execution-plane/worker/module.cpp
  src/supporting/admin/module.cpp
  src/supporting/identity/module.cpp
  src/supporting/images/module.cpp
  src/supporting/telemetry/module.cpp
  src/runtime/omnibuild/module.cpp
  src/runtime/omnirun/module.cpp
  src/runtime/omnivmm/frantz/module.cpp
  src/runtime/omnivmm/initsys/module.cpp
  src/runtime/omnivmm/meshbox/module.cpp
  src/runtime/omnivmm/omnix/module.cpp
  src/runtime/omnivmm/virtio/module.cpp
  src/extensions/lua/module.cpp
  src/extensions/native/module.cpp
)
target_include_directories(omnimesh-core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_options(omnimesh-core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(omnimesh tools/main.cpp)
target_link_libraries(omnimesh PRIVATE omnimesh-core)
add_executable(omnimesh-node-agent tools/node_agent.cpp)
target_link_libraries(omnimesh-node-agent PRIVATE omnimesh-core)

if(OMNIMESH_BUILD_TESTS)
  enable_testing()
  add_executable(omnimesh-smoke tests/unit/smoke.cpp)
  target_link_libraries(omnimesh-smoke PRIVATE omnimesh-core)
  add_test(NAME smoke COMMAND omnimesh-smoke)
endif()
if(OMNIMESH_BUILD_DOCS)
  find_package(Doxygen QUIET)
  if(Doxygen_FOUND)
    add_subdirectory(docs)
  else()
    message(STATUS "Doxygen not found; documentation target disabled")
  endif()
endif()

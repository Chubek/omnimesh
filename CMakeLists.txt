cmake_minimum_required(VERSION 3.16)
project(omnimesh VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(OMNIMESH_BUILD_TESTS "Build tests" ON)
option(OMNIMESH_BUILD_DOCS "Build documentation when Doxygen is available" OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This implementation regiment currently supports Linux only")
endif()
include(CheckCXXSymbolExists)
check_cxx_symbol_exists(posix_spawn_file_actions_addclosefrom_np "spawn.h"
  OMNIMESH_HAVE_SPAWN_CLOSEFROM)

find_package(Threads REQUIRED)
add_library(omnimesh-json INTERFACE)
target_include_directories(omnimesh-json SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann-json/include)

add_library(omnimesh-core STATIC
  src/common/status.cpp
  src/common/model.cpp
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
target_include_directories(omnimesh-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(omnimesh-core PUBLIC Threads::Threads PRIVATE omnimesh-json)
target_compile_options(omnimesh-core PRIVATE -Wall -Wextra -Wpedantic)
if(OMNIMESH_HAVE_SPAWN_CLOSEFROM)
  target_compile_definitions(omnimesh-core PRIVATE OMNIMESH_HAVE_SPAWN_CLOSEFROM=1)
else()
  message(STATUS "Safe spawn descriptor isolation unavailable; local execution disabled")
endif()

add_executable(omnimesh tools/main.cpp)
target_link_libraries(omnimesh PRIVATE omnimesh-core omnimesh-json)
target_compile_options(omnimesh PRIVATE -Wall -Wextra -Wpedantic)
add_executable(omnimesh-node-agent tools/node_agent.cpp)
target_link_libraries(omnimesh-node-agent PRIVATE omnimesh-core omnimesh-json)
target_compile_options(omnimesh-node-agent PRIVATE -Wall -Wextra -Wpedantic)

if(OMNIMESH_BUILD_TESTS)
  enable_testing()
  add_executable(omnimesh-smoke tests/unit/smoke.cpp)
  target_link_libraries(omnimesh-smoke PRIVATE omnimesh-core)
  add_test(NAME smoke COMMAND omnimesh-smoke)
  add_subdirectory(tests)
endif()
if(OMNIMESH_BUILD_DOCS)
  find_package(Doxygen QUIET)
  if(Doxygen_FOUND)
    add_subdirectory(docs)
  else()
    message(STATUS "Doxygen not found; documentation target disabled")
  endif()
endif()

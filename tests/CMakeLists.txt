foreach(suite manifest allocator scheduler orchestrator storage artifacts images)
  add_executable(omnimesh-test-${suite} unit/${suite}.cpp)
  target_link_libraries(omnimesh-test-${suite} PRIVATE omnimesh-core omnimesh-json)
  target_compile_options(omnimesh-test-${suite} PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME ${suite} COMMAND omnimesh-test-${suite})
  set_tests_properties(${suite} PROPERTIES TIMEOUT 30)
endforeach()

add_subdirectory(integration)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND OMNIMESH_HAVE_SPAWN_CLOSEFROM)
  add_executable(omnimesh-fake-runtime fixtures/fake_runtime.cpp)
  target_link_libraries(omnimesh-fake-runtime PRIVATE omnimesh-json)
  target_compile_options(omnimesh-fake-runtime PRIVATE -Wall -Wextra -Wpedantic)
  add_executable(omnimesh-test-process unit/process.cpp)
  target_link_libraries(omnimesh-test-process PRIVATE omnimesh-core)
  target_compile_options(omnimesh-test-process PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME process COMMAND omnimesh-test-process)
  set_tests_properties(process PROPERTIES TIMEOUT 15)
  add_executable(omnimesh-test-execution unit/execution.cpp)
  target_link_libraries(omnimesh-test-execution PRIVATE omnimesh-core omnimesh-json)
  target_compile_options(omnimesh-test-execution PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME execution COMMAND omnimesh-test-execution
    $<TARGET_FILE:omnimesh-fake-runtime> $<TARGET_FILE:omnimesh-node-agent>)
  set_tests_properties(execution PROPERTIES TIMEOUT 45)

  add_test(NAME agent-cli COMMAND ${CMAKE_COMMAND}
    -DCLI=$<TARGET_FILE:omnimesh>
    -DNODE_AGENT=$<TARGET_FILE:omnimesh-node-agent>
    -DFAKE_RUNTIME=$<TARGET_FILE:omnimesh-fake-runtime>
    -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
    -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/agent-fixtures
    -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/agent.cmake)
  set_tests_properties(agent-cli PROPERTIES TIMEOUT 30 RESOURCE_LOCK omnimesh_local_agent)
  add_executable(omnimesh-test-oci integration/oci.cpp)
  target_link_libraries(omnimesh-test-oci PRIVATE omnimesh-core)
  target_compile_options(omnimesh-test-oci PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME oci-runtime COMMAND omnimesh-test-oci)
  set_tests_properties(oci-runtime PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK omnimesh_local_agent)
  set_tests_properties(execution PROPERTIES RESOURCE_LOCK omnimesh_local_agent)
endif()

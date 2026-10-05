file(MAKE_DIRECTORY "${TEST_DIR}/rootfs")

function(run_agent expected)
  execute_process(COMMAND "${NODE_AGENT}" ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
  if(NOT result EQUAL expected)
    message(FATAL_ERROR "Agent exit ${result}, expected ${expected}: ${output}${errors}")
  endif()
  set(last_output "${output}" PARENT_SCOPE)
endfunction()

# Every configure/run gets a fresh private session; previous state is retained.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef session)
set(state "${TEST_DIR}/session-${session}")
file(READ "${SOURCE_DIR}/manifests/example-workload.json" workload)
string(REPLACE "/bin/echo" "/fixture/args" workload "${workload}")
file(WRITE "${TEST_DIR}/workload.json" "${workload}")
set(options --node "${SOURCE_DIR}/manifests/example-node.json"
  --runtime "${FAKE_RUNTIME}" --rootfs "${TEST_DIR}/rootfs" --state-dir "${state}")
run_agent(0 --help)
run_agent(2 run "${TEST_DIR}/workload.json")
run_agent(2 run "${TEST_DIR}/workload.json" ${options} --timeout-ms invalid)
run_agent(2 run "${TEST_DIR}/workload.json" ${options} --node duplicate)
run_agent(0 run "${TEST_DIR}/workload.json" ${options})
if(NOT last_output MATCHES "\"dryRun\": false" OR NOT last_output MATCHES "\"state\": \"Succeeded\"" OR
    NOT last_output MATCHES "\"reservationsRetained\": false")
  message(FATAL_ERROR "Invalid execution report: ${last_output}")
endif()
run_agent(3 run "${TEST_DIR}/workload.json" ${options})
if(NOT last_output MATCHES "state directory must be fresh")
  message(FATAL_ERROR "Existing runtime state was reused: ${last_output}")
endif()
string(REPLACE "/fixture/args" "/fixture/unknown" unknown "${workload}")
file(WRITE "${TEST_DIR}/unknown.json" "${unknown}")
run_agent(3 run "${TEST_DIR}/unknown.json"
  --node "${SOURCE_DIR}/manifests/example-node.json" --runtime "${FAKE_RUNTIME}"
  --rootfs "${TEST_DIR}/rootfs" --state-dir "${state}-unknown")
if(NOT last_output MATCHES "\"state\": \"Unknown\"" OR NOT last_output MATCHES "\"reservationsRetained\": true")
  message(FATAL_ERROR "Uncertain termination was not retained: ${last_output}")
endif()
# Explicit recovery of this fixture's barrier only. The fake runtime monitor has
# exited; this is not a recovery procedure for a real OCI container.
execute_process(COMMAND id -u OUTPUT_VARIABLE uid OUTPUT_STRIP_TRAILING_WHITESPACE)
set(authority "/tmp/omnimesh-local-agent-${uid}.lock")
file(READ "${authority}" barrier)
if(NOT barrier STREQUAL "${state}-unknown\n")
  message(FATAL_ERROR "Unexpected recovery barrier; leave it for its owner: ${barrier}")
endif()
file(WRITE "${authority}" "")

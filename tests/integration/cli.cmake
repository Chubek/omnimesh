file(MAKE_DIRECTORY "${TEST_DIR}")

function(run_cli expected)
  execute_process(COMMAND "${CLI}" ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL expected)
    message(FATAL_ERROR "CLI exit ${result}, expected ${expected}: ${output}${errors}")
  endif()
  set(last_output "${output}" PARENT_SCOPE)
endfunction()

run_cli(0 --version)
if(NOT last_output MATCHES "0.1.0-alpha.1")
  message(FATAL_ERROR "Version missing: ${last_output}")
endif()
run_cli(0 validate "${SOURCE_DIR}/manifests/example-workload.json")
if(NOT last_output MATCHES "\"valid\": true")
  message(FATAL_ERROR "Validation failed: ${last_output}")
endif()
run_cli(0 validate-node "${SOURCE_DIR}/manifests/example-node.json")
run_cli(0 plan "${SOURCE_DIR}/manifests/example-workload.json"
  --node "${SOURCE_DIR}/manifests/example-node.json")
set(first_plan "${last_output}")
if(NOT first_plan MATCHES "\"dryRun\": true" OR NOT first_plan MATCHES "\"complete\": true")
  message(FATAL_ERROR "Invalid plan: ${first_plan}")
endif()
string(REGEX MATCHALL "\"state\": \"Allocated\"" allocated "${first_plan}")
list(LENGTH allocated count)
if(NOT count EQUAL 2)
  message(FATAL_ERROR "Expected two independently allocated replicas: ${first_plan}")
endif()
run_cli(0 plan "${SOURCE_DIR}/manifests/example-workload.json"
  --node "${SOURCE_DIR}/manifests/example-node.json")
if(NOT first_plan STREQUAL last_output)
  message(FATAL_ERROR "Planning is not deterministic")
endif()
run_cli(3 plan "${SOURCE_DIR}/manifests/example-workload.json"
  --node "${SOURCE_DIR}/manifests/example-node.json" --quota 1000 134217728 1)
if(NOT last_output MATCHES "tenant quota exhausted" OR NOT last_output MATCHES "\"complete\": false")
  message(FATAL_ERROR "Quota not enforced in plan: ${last_output}")
endif()

file(READ "${SOURCE_DIR}/manifests/example-node.json" node)
string(REPLACE "\"verified\": true" "\"verified\": false" unverified "${node}")
file(WRITE "${TEST_DIR}/unverified.json" "${unverified}")
run_cli(3 plan "${SOURCE_DIR}/manifests/example-workload.json"
  --node "${TEST_DIR}/unverified.json")
if(NOT last_output MATCHES "not verified")
  message(FATAL_ERROR "Unverified capabilities were accepted: ${last_output}")
endif()
run_cli(2 plan "${SOURCE_DIR}/manifests/example-workload.json"
  --node "${SOURCE_DIR}/manifests/example-node.json"
  --node "${SOURCE_DIR}/manifests/example-node.json")
file(WRITE "${TEST_DIR}/invalid.json" "{\"apiVersion\":\"omnimesh.io/v9\"}")
run_cli(2 validate "${TEST_DIR}/invalid.json")
run_cli(1 validate "${TEST_DIR}/missing.json")
run_cli(2 plan "${SOURCE_DIR}/manifests/example-workload.json")
run_cli(2 unknown-command)

execute_process(COMMAND "${NODE_AGENT}" RESULT_VARIABLE result
  OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 2 OR NOT output MATCHES "explicit configuration")
  message(FATAL_ERROR "Unconfigured execution backend reported success: ${output}${errors}")
endif()

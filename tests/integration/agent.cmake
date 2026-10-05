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
# Journaled execution records facts before use; `omnimesh recover` replays them.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef journal_session)
set(journal_dir "${TEST_DIR}/journal-${journal_session}")
run_agent(0 run "${TEST_DIR}/workload.json"
  --node "${SOURCE_DIR}/manifests/example-node.json" --runtime "${FAKE_RUNTIME}"
  --rootfs "${TEST_DIR}/rootfs" --state-dir "${TEST_DIR}/session-${journal_session}"
  --journal-dir "${journal_dir}")
if(NOT last_output MATCHES "\"journaled\": true" OR NOT last_output MATCHES "\"state\": \"Succeeded\"")
  message(FATAL_ERROR "Journaled execution failed: ${last_output}")
endif()
execute_process(COMMAND "${CLI}" recover --journal-dir "${journal_dir}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
if(NOT result EQUAL 0 OR NOT output MATCHES "\"reservationsRecovered\": 2")
  message(FATAL_ERROR "Journal recovery failed: ${output}${errors}")
endif()

# Sixth regiment: the agent pins, verifies and stages an OCI layout itself.
set(image "${TEST_DIR}/image-${session}")
file(MAKE_DIRECTORY "${image}/source/bin" "${image}/layout/blobs/sha256")
file(WRITE "${image}/source/bin/hello" "verified agent image payload\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${image}/layer.tar" bin/hello
  WORKING_DIRECTORY "${image}/source" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot build agent image fixture")
endif()
file(SHA256 "${image}/layer.tar" layer_hex)
file(SIZE "${image}/layer.tar" layer_size)
file(RENAME "${image}/layer.tar" "${image}/layout/blobs/sha256/${layer_hex}")
file(WRITE "${image}/config.json"
  "{\"architecture\":\"amd64\",\"os\":\"linux\",\"rootfs\":{\"type\":\"layers\",\"diff_ids\":[\"sha256:${layer_hex}\"]}}")
file(SHA256 "${image}/config.json" config_hex)
file(SIZE "${image}/config.json" config_size)
file(RENAME "${image}/config.json" "${image}/layout/blobs/sha256/${config_hex}")
file(WRITE "${image}/manifest.json"
  "{\"schemaVersion\":2,\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\",\"config\":{\"mediaType\":\"application/vnd.oci.image.config.v1+json\",\"digest\":\"sha256:${config_hex}\",\"size\":${config_size}},\"layers\":[{\"mediaType\":\"application/vnd.oci.image.layer.v1.tar\",\"digest\":\"sha256:${layer_hex}\",\"size\":${layer_size}}]}")
file(SHA256 "${image}/manifest.json" manifest_hex)
file(SIZE "${image}/manifest.json" manifest_size)
file(RENAME "${image}/manifest.json" "${image}/layout/blobs/sha256/${manifest_hex}")
file(WRITE "${image}/layout/oci-layout" "{\"imageLayoutVersion\":\"1.0.0\"}")
file(WRITE "${image}/layout/index.json"
  "{\"schemaVersion\":2,\"mediaType\":\"application/vnd.oci.image.index.v1+json\",\"manifests\":[{\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\",\"digest\":\"sha256:${manifest_hex}\",\"size\":${manifest_size},\"platform\":{\"architecture\":\"amd64\",\"os\":\"linux\"}}]}")
string(REGEX REPLACE "sha256:[0-9a-f]+" "sha256:${manifest_hex}" image_workload "${workload}")
string(REPLACE "/fixture/args" "/fixture/image" image_workload "${image_workload}")
file(WRITE "${image}/workload.json" "${image_workload}")
set(image_options --node "${SOURCE_DIR}/manifests/example-node.json"
  --runtime "${FAKE_RUNTIME}" --image-layout "${image}/layout")
run_agent(2 run "${image}/workload.json" ${image_options}
  --state-dir "${image}/bad" --rootfs "${TEST_DIR}/rootfs")
run_agent(2 run "${image}/workload.json" ${image_options}
  --state-dir "${image}/bad" --max-image-bytes -1)
run_agent(2 run "${image}/workload.json" ${image_options}
  --state-dir "${image}/bad" --max-image-bytes 1)
run_agent(2 run "${TEST_DIR}/workload.json" ${options} --max-image-bytes 1048576)
run_agent(2 run "${TEST_DIR}/workload.json" ${image_options} --state-dir "${image}/wrong-pin")
if(EXISTS "${image}/wrong-pin/bundle-1")
  message(FATAL_ERROR "Wrong image digest reached bundle preparation")
endif()
run_agent(0 run "${image}/workload.json" ${image_options}
  --state-dir "${image}/session" --max-image-bytes 1048576)
if(NOT last_output MATCHES "\"verified\": true" OR
   NOT last_output MATCHES "\"manifestDigest\": \"sha256:${manifest_hex}\"" OR
   NOT last_output MATCHES "\"configDigest\": \"sha256:${config_hex}\"" OR
   NOT last_output MATCHES "verified agent image payload" OR
   NOT last_output MATCHES "\"state\": \"Succeeded\"")
  message(FATAL_ERROR "Verified image did not reach the worker: ${last_output}")
endif()
file(READ "${image}/session/rootfs/bin/hello" staged)
if(NOT staged STREQUAL "verified agent image payload\n")
  message(FATAL_ERROR "Staged image content mismatch")
endif()
run_agent(3 run "${image}/workload.json" ${image_options} --state-dir "${image}/session")
set(spool "${image}/spool")
file(WRITE "${image}/dataset" "verified spool input payload\n")
file(SHA256 "${image}/dataset" input_hex)
execute_process(COMMAND "${CLI}" artifact put "${image}/dataset"
  --spool-dir "${spool}" --tenant local
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot publish input fixture: ${output}${errors}")
endif()
string(REPLACE "\"spec\": {"
  "\"spec\": {\"inputs\":[{\"name\":\"dataset\",\"digest\":\"sha256:${input_hex}\"}],"
  input_workload "${image_workload}")
string(REPLACE "/fixture/image" "/fixture/inputs" input_workload "${input_workload}")
file(WRITE "${image}/input-workload.json" "${input_workload}")
run_agent(2 run "${image}/input-workload.json" ${image_options}
  --state-dir "${image}/no-spool")
run_agent(2 run "${image}/input-workload.json" ${image_options}
  --state-dir "${image}/bad-bound" --max-input-bytes 32)
run_agent(2 run "${image}/input-workload.json" ${image_options}
  --state-dir "${image}/bad-bound" --spool-dir "${spool}" --max-input-bytes -1)
run_agent(2 run "${image}/input-workload.json" ${image_options}
  --state-dir "${image}/bad-bound" --spool-dir "${spool}" --max-input-bytes 0)
run_agent(2 run "${image}/workload.json" ${image_options}
  --state-dir "${image}/unused-spool" --spool-dir "${spool}")
run_agent(0 run "${image}/input-workload.json" ${image_options}
  --state-dir "${image}/input-session" --spool-dir "${spool}" --max-input-bytes 64)
if(NOT last_output MATCHES "verified spool input payload" OR
   NOT last_output MATCHES "\"digest\": \"sha256:${input_hex}\"" OR
   NOT last_output MATCHES "/tmp/omnimesh-inputs/dataset" OR
   NOT last_output MATCHES "\"manifestDigest\": \"sha256:${manifest_hex}\"")
  message(FATAL_ERROR "Verified image and input did not reach execution: ${last_output}")
endif()
file(READ "${image}/input-session/inputs/dataset" input_staged)
if(NOT input_staged STREQUAL "verified spool input payload\n")
  message(FATAL_ERROR "Staged input content mismatch")
endif()
run_agent(3 run "${image}/input-workload.json" ${image_options}
  --state-dir "${image}/input-quota" --spool-dir "${spool}" --max-input-bytes 1)
if(NOT last_output MATCHES "\"state\": \"Cancelled\"" OR
   EXISTS "${image}/input-quota/bundle-1")
  message(FATAL_ERROR "Input quota failure reached execution: ${last_output}")
endif()
# Modifying an already-consumed layout is detected in a new session.
file(APPEND "${image}/layout/blobs/sha256/${layer_hex}" "corruption")
run_agent(2 run "${image}/workload.json" ${image_options} --state-dir "${image}/corrupt")
if(NOT last_output MATCHES "\"verified\": false" OR
   NOT last_output MATCHES "\"reservationsRetained\": false" OR
   EXISTS "${image}/corrupt/bundle-1")
  message(FATAL_ERROR "Corrupt image was used: ${last_output}")
endif()

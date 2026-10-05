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

# The third-regiment durable path: plan records facts, recover replays them.
# The CLI creates the journal directory itself with private permissions.
file(REMOVE_RECURSE "${TEST_DIR}/journal")
run_cli(0 plan "${SOURCE_DIR}/manifests/example-workload.json"
  --node "${SOURCE_DIR}/manifests/example-node.json"
  --journal-dir "${TEST_DIR}/journal")
if(NOT last_output MATCHES "\"journaled\": true" OR NOT last_output MATCHES "\"complete\": true")
  message(FATAL_ERROR "Journaled plan failed: ${last_output}")
endif()
run_cli(0 recover --journal-dir "${TEST_DIR}/journal")
if(NOT last_output MATCHES "\"reservationsRecovered\": 2")
  message(FATAL_ERROR "Recovery did not restore reservations: ${last_output}")
endif()
run_cli(2 recover)
file(REMOVE_RECURSE "${TEST_DIR}/corrupt")
file(MAKE_DIRECTORY "${TEST_DIR}/corrupt")
file(WRITE "${TEST_DIR}/corrupt/control-plane.journal" "garbage-not-a-journal-frame")
execute_process(COMMAND chmod 0700 "${TEST_DIR}/corrupt")
execute_process(COMMAND chmod 0600 "${TEST_DIR}/corrupt/control-plane.journal")
run_cli(2 recover --journal-dir "${TEST_DIR}/corrupt")
if(NOT last_output MATCHES "preserved")
  message(FATAL_ERROR "Corrupt journal was not preserved and reported: ${last_output}")
endif()

# The fourth-regiment artifact spool: publish, verify, list and collect.
file(WRITE "${TEST_DIR}/sample.txt" "hello artifacts\n")
file(REMOVE_RECURSE "${TEST_DIR}/spool")
run_cli(0 artifact put "${TEST_DIR}/sample.txt"
  --spool-dir "${TEST_DIR}/spool" --tenant local)
string(REGEX MATCH "sha256:[0-9a-f]+" sample_digest "${last_output}")
string(LENGTH "${sample_digest}" sample_length)
if(NOT sample_length EQUAL 71)
  message(FATAL_ERROR "Artifact put reported no digest: ${last_output}")
endif()
run_cli(0 artifact list --spool-dir "${TEST_DIR}/spool")
if(NOT last_output MATCHES "${sample_digest}")
  message(FATAL_ERROR "Artifact list missed the blob: ${last_output}")
endif()
run_cli(0 artifact get "${sample_digest}"
  --spool-dir "${TEST_DIR}/spool" --tenant local --out "${TEST_DIR}/fetched.txt")
file(READ "${TEST_DIR}/sample.txt" sample_bytes)
file(READ "${TEST_DIR}/fetched.txt" fetched_bytes)
if(NOT sample_bytes STREQUAL fetched_bytes)
  message(FATAL_ERROR "Verified fetch changed the bytes")
endif()
run_cli(3 artifact get "${sample_digest}"
  --spool-dir "${TEST_DIR}/spool" --tenant other --out "${TEST_DIR}/nope.txt")
if(NOT last_output MATCHES "another tenant")
  message(FATAL_ERROR "Cross-tenant fetch was not denied: ${last_output}")
endif()
run_cli(0 artifact gc --spool-dir "${TEST_DIR}/spool" --keep "${sample_digest}")
if(NOT last_output MATCHES "\"removed\": 0")
  message(FATAL_ERROR "Kept artifact was collected: ${last_output}")
endif()
run_cli(0 artifact gc --spool-dir "${TEST_DIR}/spool")
if(NOT last_output MATCHES "\"removed\": 1")
  message(FATAL_ERROR "Unreferenced artifact was not collected: ${last_output}")
endif()
run_cli(2 artifact put "${TEST_DIR}/sample.txt" --spool-dir "${TEST_DIR}/spool")
run_cli(2 artifact bogus --spool-dir "${TEST_DIR}/spool")

# The fifth-regiment image loader: build a layout with cmake, verify by CLI.
file(MAKE_DIRECTORY "${TEST_DIR}/image/rootfs/bin")
file(WRITE "${TEST_DIR}/image/rootfs/bin/hello" "#!/bin/sh\necho hi\n")
file(MAKE_DIRECTORY "${TEST_DIR}/image/layout/blobs/sha256")
execute_process(COMMAND ${CMAKE_COMMAND} -E chdir "${TEST_DIR}/image/rootfs"
  ${CMAKE_COMMAND} -E tar cf "${TEST_DIR}/image/layer.tar" bin/hello
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot build fixture layer")
endif()
file(SHA256 "${TEST_DIR}/image/layer.tar" layer_hex)
file(SIZE "${TEST_DIR}/image/layer.tar" layer_size)
file(RENAME "${TEST_DIR}/image/layer.tar"
  "${TEST_DIR}/image/layout/blobs/sha256/${layer_hex}")
set(layer_digest "sha256:${layer_hex}")
set(layer_diff "${layer_digest}")
file(WRITE "${TEST_DIR}/image/config.json"
  "{\"architecture\":\"amd64\",\"os\":\"linux\",\"rootfs\":{\"type\":\"layers\",\"diff_ids\":[\"${layer_diff}\"]}}")
file(SHA256 "${TEST_DIR}/image/config.json" config_hex)
file(SIZE "${TEST_DIR}/image/config.json" config_size)
file(RENAME "${TEST_DIR}/image/config.json"
  "${TEST_DIR}/image/layout/blobs/sha256/${config_hex}")
set(config_digest "sha256:${config_hex}")
file(WRITE "${TEST_DIR}/image/manifest.json"
  "{\"schemaVersion\":2,\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\",\"config\":{\"mediaType\":\"application/vnd.oci.image.config.v1+json\",\"digest\":\"${config_digest}\",\"size\":${config_size}},\"layers\":[{\"mediaType\":\"application/vnd.oci.image.layer.v1.tar\",\"digest\":\"${layer_digest}\",\"size\":${layer_size}}]}")
file(SHA256 "${TEST_DIR}/image/manifest.json" manifest_hex)
file(SIZE "${TEST_DIR}/image/manifest.json" manifest_size)
file(RENAME "${TEST_DIR}/image/manifest.json"
  "${TEST_DIR}/image/layout/blobs/sha256/${manifest_hex}")
set(manifest_digest "sha256:${manifest_hex}")
file(WRITE "${TEST_DIR}/image/layout/oci-layout" "{\"imageLayoutVersion\":\"1.0.0\"}")
file(WRITE "${TEST_DIR}/image/layout/index.json"
  "{\"schemaVersion\":2,\"mediaType\":\"application/vnd.oci.image.index.v1+json\",\"manifests\":[{\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\",\"digest\":\"${manifest_digest}\",\"size\":${manifest_size},\"platform\":{\"architecture\":\"amd64\",\"os\":\"linux\"}}]}")
run_cli(0 image inspect --layout "${TEST_DIR}/image/layout"
  --platform linux/amd64 --digest "${manifest_digest}")
if(NOT last_output MATCHES "${layer_digest}")
  message(FATAL_ERROR "Image inspect missed the layer: ${last_output}")
endif()
file(REMOVE_RECURSE "${TEST_DIR}/image/rootfs-out")
run_cli(0 image unpack --layout "${TEST_DIR}/image/layout"
  --rootfs "${TEST_DIR}/image/rootfs-out" --platform linux/amd64
  --digest "${manifest_digest}")
file(READ "${TEST_DIR}/image/rootfs-out/bin/hello" hello_out)
if(NOT hello_out STREQUAL "#!/bin/sh\necho hi\n")
  message(FATAL_ERROR "Unpacked rootfs content mismatch: ${hello_out}")
endif()
run_cli(3 image unpack --layout "${TEST_DIR}/image/layout"
  --rootfs "${TEST_DIR}/image/rootfs-out")
run_cli(2 image unpack --layout "${TEST_DIR}/image/layout")
run_cli(2 image bogus --layout "${TEST_DIR}/image/layout")
run_cli(1 image inspect --layout "${TEST_DIR}/missing")

execute_process(COMMAND "${NODE_AGENT}" RESULT_VARIABLE result
  OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 2 OR NOT output MATCHES "explicit configuration")
  message(FATAL_ERROR "Unconfigured execution backend reported success: ${output}${errors}")
endif()

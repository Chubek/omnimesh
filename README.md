# OmniMesh

OmniMesh is a distributed execution platform built around OCI-compatible
container execution and image distribution. This repository is at the end of its
first implementation regiment: the control-plane planning path is implemented
and tested, while execution is deliberately not.

## Build and test

Requires CMake 3.16+ and a C++17 compiler. Ninja is optional.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DOMNIMESH_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Test suites: `manifest`, `allocator`, `scheduler`, `orchestrator` (unit) and
`cli` (integration, drives the built binaries).

## What works

Strict manifest validation, deterministic placement, tenant-aware atomic
reservation, and task/attempt reconciliation are implemented in
`src/control-plane/`:

| Command | Purpose |
| --- | --- |
| `omnimesh --version` | Print the version. |
| `omnimesh validate WORKLOAD.json` | Validate a workload manifest. |
| `omnimesh validate-node NODE.json` | Validate a node inventory document. |
| `omnimesh plan WORKLOAD.json --node NODE.json [...] [--quota CPU MEM MAX_ALLOC]` | Admission plus placement dry run. |

Every command emits JSON on stdout and returns `0` on success, `2` for invalid
input or usage, `3` for unavailable or denied operations, and `1` for internal
failures.

```sh
omnimesh plan manifests/example-workload.json --node manifests/example-node.json
```

`plan` reports per-task state and, when a task cannot be placed, the reasons
each node was rejected. It reports `dryRun: true` and never starts work.

Manifests are **strict JSON**, which is also valid YAML 1.2. General YAML
syntax is rejected rather than partially understood, so
`manifests/example-workload.yaml` was removed in favor of the `.json` examples;
a tag-based or partially parsed workload is a silent correctness risk.

Contracts are documented in `manifests/workload.schema.yaml` and
`manifests/node.schema.yaml`, which the C++ decoder enforces.

## What does not work

`omnimesh-node-agent` reports `unavailable` and exits non-zero. There is no OCI
lifecycle, container creation, artifact transfer, network transport,
authentication, durable storage, node discovery, membership, telemetry, VMM,
Omnix, Initsys, Meshbox, Frantz, native plugin loading, or Lua execution.
`omnimesh-core` is a static library; the plugin headers in `include/` remain a
declared interface with no loader.

`WorkloadController` and `Allocator` are process-local and hold all state in
memory. There is no persistence, no controller failover, and no lease or fencing
token, so multiple processes cannot safely share authority. See
`docs/status.md`.

## Repository conventions

Sources live under `src/` and public headers under `include/`, mirroring the
subsystem layout. Manifest examples and schemas live under `manifests/`; paths
in documentation and tooling are relative to the repository root.

Vendored third-party code lives under `third_party/` and is committed. CMake
never downloads anything. `third_party/nlohmann-json` is a pinned MIT-licensed
single header used only for manifest decoding and CLI output, with recorded
SHA-256 digests in its README.
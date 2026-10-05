# OmniMesh

OmniMesh is a distributed execution platform built around OCI-compatible
container execution and image distribution. The second implementation
regiment adds experimental local OCI execution to the tested control-plane
planning path. See [the regiment contract](docs/second-regiment.md).

## Build and test

Requires Linux, CMake 3.16+ and a C++17 compiler. Ninja is optional. Execution
requires libc support for `posix_spawn_file_actions_addclosefrom_np` (verified on
glibc), an administrator-installed runtime and its required host capabilities.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DOMNIMESH_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Test suites: `smoke`, `manifest`, `allocator`, `scheduler`, `orchestrator`, `storage`,
`process`, `execution`, `cli`, and `agent-cli`. The optional `oci-runtime` test
skips unless a real runtime and rootfs are explicitly configured.

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

Validation, planning and execution results emit JSON on stdout. Commands return
`0` on success, `2` for invalid input or usage, `3` for unavailable or denied
operations, and `1` for internal failures. Version and help output are plain text.

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

## Local execution

```sh
build/omnimesh-node-agent run manifests/example-workload.json \
  --node manifests/example-node.json \
  --runtime /usr/bin/crun --rootfs /absolute/path/to/provisioned-rootfs \
  --state-dir /absolute/path/to/fresh-session
```

The foreground agent supervises one worker at a time, preserves retry history,
captures bounded output and handles cancellation. It confirms runtime termination
before releasing reservations. Uncertain termination blocks further sessions for
the host user until manual recovery. The administrator supplies and verifies the
rootfs; image digest annotations do not verify its contents. This profile is
experimental and has no OCI conformance claim. See
[execution and recovery details](docs/second-regiment.md).

## Remaining boundaries

Image loading, artifact transfer, distributed transports, authentication,
discovery, membership, telemetry, controller failover, VMM, Omnix, Initsys,
Meshbox, Frantz, native plugin loading and Lua execution remain unimplemented.
`omnimesh-core` is a static library; the plugin headers declare an interface
with no loader.

`WorkloadController` and `Allocator` remain process-local. A separately callable,
experimental journal can reconstruct recorded control-plane facts; neither CLI
uses it and no API transactionally journals their live mutations. Local execution
uses a host-user lock and conservative recovery barrier. There are no distributed
leases or fencing tokens. See [implementation status](docs/status.md).

## Repository conventions

Sources live under `src/` and public headers under `include/`, mirroring the
subsystem layout. Manifest examples and schemas live under `manifests/`; paths
in documentation and tooling are relative to the repository root.

Vendored third-party code lives under `third_party/` and is committed. CMake
never downloads anything. `third_party/nlohmann-json` is a pinned MIT-licensed
single header used for manifests, runtime state, journal payloads and CLI output,
with recorded SHA-256 digests in its README.

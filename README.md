# OmniMesh

OmniMesh is a distributed execution platform built around OCI-compatible
container execution and image distribution. The sixth implementation
regiment connects digest-verified local OCI layouts directly to node-agent
execution, with bounded staging and cancellation before launch.
See [the regiment contract](docs/sixth-regiment.md).

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
`process`, `execution`, `artifacts`, `images`, `cli`, and `agent-cli`. The optional `oci-runtime` test
skips unless a real runtime and rootfs are explicitly configured. Journaled planning
and execution are covered by cases in `execution`, `cli` and `agent-cli`; the spool
is covered by `artifacts` and `artifact` cases in `cli`; image loading is covered
by `images` and `image` cases in `cli`. See
[the durable-execution contract](docs/third-regiment.md),
[the spool contract](docs/fourth-regiment.md) and
[the image contract](docs/fifth-regiment.md) and
[verified image execution](docs/sixth-regiment.md).

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
| `omnimesh plan ... --journal-dir DIR` | Dry run that also records planning facts for later recovery. |
| `omnimesh recover --journal-dir DIR` | Replay a journal into fresh memory and report the outcome. |
| `omnimesh artifact put FILE --spool-dir DIR --tenant TENANT` | Hash, verify and publish a content-addressed blob. |
| `omnimesh artifact get DIGEST --spool-dir DIR --tenant TENANT --out FILE` | Re-verify and fetch a blob by digest. |
| `omnimesh artifact list --spool-dir DIR` | List spool contents, usage and recovery notes. |
| `omnimesh artifact gc --spool-dir DIR [--keep DIGEST ...]` | Remove unreferenced blobs and report reclaimed bytes. |
| `omnimesh image inspect --layout DIR [--platform OS/ARCH] [--digest DIGEST]` | Validate a local OCI layout and report the selected manifest. |
| `omnimesh image unpack --layout DIR --rootfs OUT [--platform OS/ARCH] [--digest DIGEST]` | Verify and extract layers into a fresh rootfs. |

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
  --state-dir /absolute/path/to/fresh-session \
  --journal-dir /absolute/path/to/journal
```

The foreground agent supervises one worker at a time, preserves retry history,
captures bounded output and handles cancellation. It confirms runtime termination
before releasing reservations. Uncertain termination blocks further sessions for
the host user until manual recovery. With `--journal-dir` it records
control-plane facts before acting on them and fails closed on recording errors;
`omnimesh recover` replays the journal without restarting work. With `--rootfs`,
the administrator supplies and verifies the tree; image digest annotations do
not verify its contents. This profile is
experimental and has no OCI conformance claim. See
[execution and recovery details](docs/third-regiment.md).

To prepare the rootfs directly from an OCI layout, replace `--rootfs` with
`--image-layout /absolute/path/to/layout`. The workload's `spec.image` must
pin that layout's selected **manifest** digest. The agent verifies and unpacks
into `STATE_DIR/rootfs` before reserving resources or invoking the runtime;
replicas and retries share the read-only tree. `--max-image-bytes` defaults
to 1 GiB, and the session timeout includes preparation. Results report the
rootfs source, verification status and verified digests. See
[the sixth-regiment contract](docs/sixth-regiment.md).

## Artifact spool

```sh
omnimesh artifact put input.bin --spool-dir /absolute/path/to/spool --tenant local
omnimesh artifact get sha256:<64 hex> --spool-dir /absolute/path/to/spool \
  --tenant local --out output.bin
omnimesh artifact list --spool-dir /absolute/path/to/spool
omnimesh artifact gc --spool-dir /absolute/path/to/spool --keep sha256:<64 hex>
```

The local spool publishes immutable content-addressed blobs, re-hashes on
every fetch, enforces tenant labels and quotas, and collects unreferenced
blobs on demand. Tampered blobs fail closed and are preserved. Workload
manifests declare no artifact inputs and nothing stages spool blobs into
workers; registry access and cross-node transfer remain later work. See
[the spool contract](docs/fourth-regiment.md).

## Image loading

```sh
omnimesh image unpack --layout /absolute/path/to/layout \
  --rootfs /absolute/path/to/fresh-rootfs --platform linux/amd64
```

Local OCI layouts use `blobs/sha256/<hex>` and validate platform selection with optional digest
pinning, per-blob digest verification while streaming, diff-ID chain checks,
and root-confined extraction that refuses escapes, strips privileges and
honors whiteouts. The administrator can provision `--rootfs` from a verified
unpack instead of by hand, or use the agent's `--image-layout` path. No registry,
signatures, Docker formats or zstd layers. Earlier flat `blobs/<hex>` layouts
must be migrated; descriptor sizes are now checked. See
[the image contract](docs/fifth-regiment.md) and
[compatibility corrections](docs/sixth-regiment.md#oci-layout-correction-and-compatibility).

## Remaining boundaries

Registry access, image building, artifact transfer, distributed transports, authentication,
discovery, membership, telemetry, controller failover, VMM, Omnix, Initsys,
Meshbox, Frantz, native plugin loading and Lua execution remain unimplemented.
`omnimesh-core` is a static library; the plugin headers declare an interface
with no loader.

`WorkloadController` and `Allocator` remain process-local. The CLIs record
journaled facts before acting on them and `omnimesh recover` replays a journal
into fresh memory, but live mutations are not transactionally wrapped inside
their locks. Local execution
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

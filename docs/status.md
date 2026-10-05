# Implementation status

This file distinguishes implemented behavior from planned behavior. Implemented
protocol behavior is exercised by tests; real OCI compatibility requires the
optional runtime test described in `docs/second-regiment.md`.

## Implemented (v1alpha1, experimental)

**Manifest decoding and admission** — `src/control-plane/admission/module.cpp`.
Strict JSON decoding with a 1 MiB size limit, 32-level nesting limit and 16384
token limit; duplicate object keys, trailing content and non-JSON input are
rejected. Field paths and actionable messages are reported, unknown fields are
errors, and up to 64 diagnostics are returned. Rejects mutable tags, `privileged:
true`, non-OCI runtimes, non-Linux platforms, and out-of-range resources,
replicas and retry budgets. Secret and environment injection are unsupported.

**Resource model and fit arithmetic** — `src/common/model.cpp`. `fits()` checks
subtraction after validating usage, so `UINT64_MAX` capacity cannot overflow into
a false fit.

**Allocator** — `src/control-plane/allocator/module.cpp`. Authoritative
reservation accounting under a mutex: tenant quotas, per-node capacity,
duplicate-safe reservations keyed by attempt identity, bounded history with
tombstones so a released attempt cannot be resurrected, tenant-scoped release and
inspect, and quota/capacity changes that cannot strand active reservations.
Covered by a 32-thread reservation race test.

**Scheduler** — `src/control-plane/scheduler/module.cpp`. Hard filters for
readiness, verification, tenant authorization, platform, runtime, capability and
label requirements, and resource fit. Ranks remaining CPU, then remaining
memory, then lexicographic node ID, so placement is deterministic. Emits bounded
per-node rejection reasons. Proposing a placement reserves nothing and
acknowledges no execution.

**Orchestrator** — `src/control-plane/orchestrator/module.cpp`. Task and attempt
records with a monotonic desired-state generation; replicas expand into
independently reserved tasks; retry budgets create new attempt records without
overwriting history; idempotent reconcile; stale generation, stale sequence and
reused sequence detection; `Unknown` observations for uncertain attempts;
rejection of reservations on nodes that became ineligible between scheduling and
start; cancellation that retains reservations for `Starting`, `Running` and
`Unknown` attempts until termination is confirmed, and immediate release for
`Allocated` attempts. Bounded by 128 workloads, 4096 tasks and 8192 allocation
records. Backoff uses monotonic time with stable per-attempt jitter.

**CLI** — `tools/main.cpp`. `validate`, `validate-node` and `plan` with JSON
output and documented exit codes.

**Local execution and worker supervision** — `src/execution-plane/agent/module.cpp`,
`src/execution-plane/worker/module.cpp`, `src/runtime/omnirun/module.cpp` and
`tools/node_agent.cpp`. Generates private OCI 1.0.2 bundles from trusted
pre-provisioned rootfs directories, invokes an explicit runc/crun CLI protocol,
queues replicas, reconciles runtime observations, retains retry history, captures
bounded output, and handles cancellation with TERM/KILL deadlines. A matching
runtime stopped state is required before resource release. A host-user authority
lock and recovery barrier prevent overlapping sessions and automatic restart
following uncertain execution. Tested with a compiled protocol fixture; real
runtime compatibility and resource enforcement remain unverified when the
optional `oci-runtime` suite skips.

**Experimental control-plane journal** — `src/control-plane/storage/module.cpp`
and `src/control-plane/api/module.cpp`. Single-writer, versioned, checksummed,
fsynced records with 64 KiB payload and 16 MiB file limits. Repairs incomplete
final writes; complete corruption and invalid or unknown event payloads fail
closed. Restores recorded reservations as Unknown with accounting retained;
historic liveness events do not authorize execution. This is a manually invoked
library interface, not a transactional integration into live controller mutations.
Neither CLI uses it.

## Not implemented

OCI image loading, registry access, digest verification and conformance;
artifact and spool storage; networking and message passing; discovery, enrollment and membership; authentication, authorization at
the API boundary and credential rotation; transactionally journaled desired-state
mutations and distributed coordination storage; controller failover, leader election, leases and fencing tokens;
telemetry, metrics, logs and audit sinks; the OMNI score and heuristic ranking;
priority, fairness and preemption; gang scheduling; image build, import and
export; identity enrollment; the plugin ABI loader and the sandboxed Lua engine;
a standalone Omnirun command, Omnibuild, OmniVMM, Omnix, Initsys, Meshbox and
Frantz.

## Known limitations

- `Allocator` and `WorkloadController` are process-local and hold state in
  memory. Manually journaled facts can be reconstructed, but the journal does
  not wrap their live mutations. Local execution serializes sessions for one
  host UID. Multiple users, isolated `/tmp` mounts and distributed controllers
  do not share authoritative accounting safely.
- Caller identities passed to the controller and the `tenant` arguments are
  trusted input from a future authenticated adapter. There is no tenant
  isolation at this boundary; it is an in-process interface, not an API.
- Node inventory is administrator-supplied input, not discovered or enrolled
  membership. Discovery must never grant membership.
- Manifests are strict JSON only; YAML is accepted only insofar as JSON is valid
  YAML 1.2.
- Retry jitter is a deterministic hash of the attempt ID, not a random draw.
  This keeps tests reproducible and does not de-synchronize real backoff, but it
  is not a uniform distribution.
- The controller alone does not enforce termination deadlines; the local agent
  does. Unconfirmed termination retains reservations and the recovery barrier.
  Containers may survive an agent crash and require manual recovery.
- Local execution requires Linux and libc descriptor-close support for spawn.
  Rootless resource enforcement requires host cgroup delegation and a capable
  runtime. Unsupported limits are not removed from the generated bundle.
- Local rootfs contents and their relationship to the requested image digest are
  administrator-verified. The agent records an annotation, not digest proof.
- The journal has no compaction, distributed lease, fencing, automated recovery
  or CLI integration. Failed recovery state must not be used for scheduling.
- `Workload` updates are rejected with `conflict` rather than reconciled.
- There is no conformance claim against any OCI specification version.
- The plugin headers in `include/OmniMesh-Plugin.h` and
  `include/OmniMesh-Plugin.hpp` declare an interface with no loader behind it.

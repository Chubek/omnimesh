# Implementation status

This file distinguishes implemented behavior from planned behavior. Only what is
listed under Implemented has been exercised by the test suite.

## Implemented (v1alpha1, experimental)

**Manifest decoding and admission** — `src/control-plane/admission/module.cpp`.
Strict JSON decoding with a 1 MiB size limit, 32-level nesting limit and 16384
token limit; duplicate object keys, trailing content and non-JSON input are
rejected. Field paths and actionable messages are reported, unknown fields are
errors, and up to 64 diagnostics are returned. Rejects mutable tags, `privileged:
true`, non-OCI runtimes, non-Linux platforms, and out-of-range resources,
replicas and retry budgets. Secrets are referenced only; no field embeds one.

**Resource model and fit arithmetic** — `src/common/model.cpp`. `fits()` checks
subtraction after validating usage, so `UINT64_MAX` capacity cannot overflow into
a false fit.

**Allocator** — `src/control-plane/allocator/module.cpp`. Authoritative
reservation accounting under a mutex: tenant quotas, per-node capacity,
duplicate-safe reservations keyed by attempt identity, bounded history with
tombstones so a released attempt cannot be resurrected, tenant-scoped release and
inspect, and quota/capacity changes that cannot strand active reservations.
Verified with a 32-thread concurrent test under ThreadSanitizer.

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
`Allocated` attempts. Bounded by 256 workloads, 4096 tasks and 8192 allocation
records. Backoff uses monotonic time with stable per-attempt jitter.

**CLI** — `tools/main.cpp`. `validate`, `validate-node` and `plan` with JSON
output and documented exit codes.

## Not implemented

OCI lifecycle and image handling; container creation or any runtime backend
invocation; the node agent; artifact and spool storage; networking and message
passing; discovery, enrollment and membership; authentication, authorization at
the API boundary and credential rotation; durable desired-state or coordination
storage; controller failover, leader election, leases and fencing tokens;
telemetry, metrics, logs and audit sinks; the OMNI score and heuristic ranking;
priority, fairness and preemption; gang scheduling; image build, import and
export; identity enrollment; the plugin ABI loader and the sandboxed Lua engine;
Omnirun, Omnibuild, OmniVMM, Omnix, Initsys, Meshbox and Frantz.

## Known limitations

- `Allocator` and `WorkloadController` are process-local and hold state in
  memory. Nothing survives a restart and no two processes share authority
  safely. Running two control planes against one store would overcommit.
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
- Cancellation has no deadline or forced-termination enforcement. A worker that
  never reports termination keeps its reservation indefinitely.
- `Workload` updates are rejected with `conflict` rather than reconciled.
- There is no conformance claim against any OCI specification version.
- The plugin headers in `include/OmniMesh-Plugin.h` and
  `include/OmniMesh-Plugin.hpp` declare an interface with no loader behind it.
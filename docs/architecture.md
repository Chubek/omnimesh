# OmniMesh architecture

The scaffold separates control plane (desired state, admission, scheduling and
allocation) from execution plane (node agents, workers, resources and
monitoring). Supporting services, runtimes, extensions and common interfaces
remain independently replaceable.

Invariants: unsupported capabilities are rejected or reported explicitly; desired
and observed state are distinct; plugin ABI calls return statuses and never leak
exceptions; resource reservations do not imply execution.

## Implemented control-plane path

```
manifest JSON ─▶ admission ─▶ task fan-out ─▶ scheduler ─▶ allocator ─▶ attempt
                    │             │                                            │
              diagnostics     desired state                        authoritative
                             (generation)                          reservation
```

`admission` decodes and validates a workload. `WorkloadController` expands a
workload generation into one task per replica and reconciles each task toward
`Allocated`. `propose_placement` only ranks eligible nodes; `Allocator::reserve`
is the single authoritative commit point and re-verifies every hard constraint,
because scheduling and reservation happen under separate locks and node state
may change in between.

`begin_start` issues the start intent and `observe` records agent
acknowledgments. A reservation alone never advances a task to `Running`.

### State separation

Desired state (`Workload.spec`, `TaskRecord`) is distinct from observed state
(`AttemptRecord`, `AttemptState`). `AttemptState::unknown` exists because an
unreachable node does not prove an attempt stopped: the reservation is retained
and the attempt is never silently declared failed.

### Idempotency and ordering

Reservations are keyed by attempt identity, so retries and duplicate deliveries
are safe. Observations carry the allocation's node, desired-state generation and
a strictly increasing sequence. Duplicate sequences must be identical,
sequences equal to zero are rejected, and terminal attempts are immutable.

### Resource accounting

Requests, reservations, limits and measured usage are distinct. `fits()`
subtracts after validating usage so `UINT64_MAX` capacity cannot overflow into a
false fit. Tenant quotas and node capacity are checked inside the allocator's
lock, so concurrent placement cannot overcommit a non-shareable resource.

### Bounded consumption

Node inventory (256), workloads (128), tasks (4096), allocations (8192), replica
count (256), retries (16), backoff (60000 ms), manifest size (1 MiB), nesting
(32), tokens (16384), diagnostics (64) and reported node rejections are all
explicitly bounded.

## Not implemented

The execution plane, artifact and spool storage, networking, discovery and
membership, API boundary authentication, durable storage, controller failover,
telemetry, extensions, and the Omnirun/Omnibuild/OmniVMM/Omnix/Initsys/Meshbox
runtime remain unimplemented. `WorkloadController` and `Allocator` are
process-local with no persistence, leases or fencing tokens; running two control
planes over shared state would overcommit resources. `docs/status.md` records
these limitations.
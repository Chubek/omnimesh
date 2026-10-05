# OmniMesh architecture

The implementation separates control plane (desired state, admission, scheduling and
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

## Local execution path

```
node-agent CLI -> controller -> allocator -> private OCI bundle -> runtime run
                     ^                                  |             |
                     +---- ordered state observations <-+--- state ---+
```

`execute_local` operates on trusted local inventory and a provisioned rootfs.
It reserves one worker at a time and issues `begin_start` before invoking the
runtime. Runtime state provides execution acknowledgment. A stopped state must
match the container identity before terminal observation releases accounting.
Cancellation updates desired state before TERM/KILL; uncertain termination
retains accounting and blocks another session for the host user.

`ChildProcess` owns its subprocess and uses direct argv, a minimal environment,
closed inherited descriptors, nonblocking bounded output capture and bounded
reaping. Runtime control commands have two-second deadlines. Each session
uses a new private directory; a synced marker in the authority lock file acts
as a manual recovery barrier after process death. This is local coordination,
not distributed fencing or a durable allocation database.

## Journal boundary

`Journal` provides bounded, single-writer version-one event framing and rejects
complete corruption before replay. `DurableControlPlane` revalidates manifests
and restores manually recorded reservations as Unknown. Neither CLI uses this
interface; live controller mutations are not transactionally journaled. Recovery
failure forbids scheduling against its partially reconstructed state.

## Planned subsystems

Artifact and spool storage, OCI image loading, distributed networking, discovery
and membership, API authentication, transactional persistent controllers,
controller failover, telemetry, extensions, and Omnibuild/OmniVMM/Omnix/Initsys/
Meshbox remain unimplemented. The Omnirun boundary now contains a local OCI
runtime adapter. `docs/status.md` and `docs/second-regiment.md` document tested
behavior, compatibility limitations and manual recovery.

# AGENTS.md — OmniMesh

This file defines the architectural intent, engineering conventions, and
contributor guidance for OmniMesh. It applies to the entire repository unless
a more specific `AGENTS.md` overrides it within a subdirectory.

This document describes the target architecture, not a guarantee that every
subsystem is implemented. Source code, tests, and explicitly documented
interfaces establish current behavior. Clearly distinguish implemented,
experimental, and planned features.

## 1. Project Overview

OmniMesh is a distributed execution platform built around OCI-compatible
container execution and image distribution.

An OmniMesh workload definition describes both:

- The environment in which an application executes.
- How its tasks are placed, connected, monitored, and recovered across CPU
  cores, GPUs, and interconnected computers.

OmniMesh targets deployments ranging from a single workstation to clusters
connected through local, ad-hoc, or wide-area networks.

OmniMesh coordinates application processes; it does not automatically turn
arbitrary single-process applications into distributed programs. Applications
must expose parallel work, use supported execution adapters, or participate
in an explicit distributed execution protocol.

### Standards and compatibility

Keep these responsibilities distinct:

- **OCI Runtime Specification:** container configuration, lifecycle, and
  execution semantics.
- **OCI Image Specification:** image manifests, configuration, layers, and
  content-addressed image layout.
- **OCI Distribution Specification:** registry interactions, when supported.
- **OmniMesh workload specification:** distributed placement, dependencies,
  resource requirements, communication, and recovery policies.

Distributed execution settings belong in the OmniMesh workload specification.
Do not silently redefine standard OCI fields.

Document supported specification versions, operating systems, architectures,
runtime backends, and any compatibility limitations. Claim conformance only
where it has been verified.

## 2. Engineering Principles

1. **Correctness before optimization.**
   Scheduling and recovery must preserve declared workload semantics.

2. **Explicit capabilities over assumptions.**
   Discover and validate hardware, runtime, network, and security capabilities.

3. **Least privilege by default.**
   Grant only the resources and permissions required by an authorized task.

4. **Local autonomy with coordinated control.**
   Nodes maintain safe local operation during control-plane interruptions.

5. **Reconciliation rather than one-shot commands.**
   Controllers repeatedly converge observed state toward desired state.

6. **Idempotent distributed operations.**
   Retries, duplicate messages, and interrupted requests are normal conditions.

7. **Bounded resource consumption.**
   Queues, retries, caches, logs, and network buffers require explicit limits.

8. **Observable decisions.**
   Placement, rejection, eviction, and recovery decisions should be explainable.

9. **Stable interfaces.**
   Version externally visible APIs, protocols, persisted records, and schemas.

10. **Honest feature boundaries.**
    Reject unsupported requirements instead of silently weakening them.

## 3. Terminology

- **Omnirun:** the runtime part of the OCI-conformant runtime.
- **Omnibuild:** the image builder.
- **Cluster:** a logical administrative and scheduling domain.
- **Node:** a machine or virtual machine running an OmniMesh node agent.
- **Node agent:** the daemon responsible for local execution and enforcement.
- **Workload:** a submitted application or computation and its desired state.
- **Task:** the smallest independently scheduled unit of work.
- **Attempt:** one execution of a task; retries create new attempts.
- **Worker:** a supervised execution environment for a task attempt, normally
  backed by one container or an explicitly defined container group.
- **Allocation:** a reservation of node resources for an attempt or task group.
- **Artifact:** immutable input or output data identified by a digest.
- **Lease:** a time-bounded claim on ownership or resource use.
- **Generation:** a monotonically increasing version of desired state.
- **Tenant:** an administrative boundary for identity, quotas, and isolation.
- **Kernel:** users can embed a POSIX-conformat kernel in their containers. This kernel is call Omnix.
- **OmniVMM:** the hypervisor for Omnix-based containers.
- **Initsys:** the init process system for OmniMesh containers, works with kernel as well.
- **Meshbox:** a collection of POSIX utilities loaded into the kernel-based containers by Initsys.
- **Frantz:** is the invisible EFI-like firmware that handles the VMM mode. It mostly handles booting.

Do not use “node,” “worker,” and “task” interchangeably.

## 4. System Architecture

OmniMesh consists of cooperating subsystems. Long-lived coordination and
supervision components normally run as daemons. Libraries, command-line
tools, image builders, and short-lived helpers do not need to be daemons.

Single-node deployments may combine components in one process. Larger
deployments may separate them, provided interface and security boundaries
remain explicit.

If the user launches a container that has our Omnix kernel embedded in it, the
container will become a hybrid supervisor-container runtime. It will then
be completely divorced of the operating system's resources, and act as a
VMM (Virtual Machine Monitor). In this mode, the runtime will rely on
virtual hardware through Virtio. We ship OmniMesh with its own
Virtio implementation. The OmniMesh kernel also implements Virtio.

OmniMesh-Virtio has two sides. Host and guest. The system running
the hybrid kernel-container will have the Host Virtio, and the
kernel has Guest Virtio loaded by default.

OmniVMM has four modes:
- KVM Mode: relies on KVM assistance for virtualization
- BPF Mode: relies on eBPF assistance for vitualization
- XED Mode: relies on Intel XED assistance for virtualization
- Virtual Mode: assistance-free mode. Relies on its own internal hypervisor.

### 4.1 Control plane

The control plane contains:

- An authenticated API and admission layer.
- A workload orchestrator.
- A scheduler.
- An allocator and policy engine.
- A node membership and capability registry.
- Durable desired-state and coordination storage.

The control plane accepts workload definitions, validates policy, computes
placement, and reconciles execution.

### 4.2 Execution plane

Each execution node contains or integrates:

- A node agent and OCI runtime backend.
- Per-attempt worker supervision.
- Resource and device enforcement.
- Artifact storage and a data spool.
- Network and message-passing facilities.
- Health, security, bandwidth, and resource monitoring.

### 4.3 Supporting services

Supporting services include:

- Image creation, validation, import, export, and distribution.
- Identity enrollment and credential rotation.
- Metrics, logs, traces, and audit collection.
- Administrative and diagnostic tooling.

These services may be embedded, external, or optional according to the
deployment profile.

## 5. Subsystem Responsibilities

### 5.1 Container runtime and node manager

The node agent:

- Registers node identity and advertises verified capabilities.
- Discovers CPUs, memory, accelerators, storage, and network interfaces.
- Prepares OCI bundles and invokes a supported runtime backend.
- Creates, starts, stops, and removes containers.
- Enforces resource limits and isolation policy.
- Tracks allocations, attempts, and local execution state.
- Reports health and execution events.
- Drains safely for maintenance and shutdown.
- Reconciles runtime state after agent restarts.

Prefer established OCI runtime implementations where practical rather than
duplicating low-level container isolation machinery.

### 5.2 Per-container workers

Workers execute task attempts and report progress and results.

Every attempt must have a unique identity associated with its workload,
task, allocation, and desired-state generation.

Worker supervision must handle:

- Startup and readiness.
- Graceful termination and forced termination deadlines.
- Exit status and failure classification.
- Output capture with bounded buffering.
- Cleanup of temporary resources.
- Optional checkpoints and progress reports.

A worker must not independently expand its privileges or resource allocation.

### 5.3 Orchestrator

The orchestrator manages workload-level desired state:

- Dependency graphs and task readiness.
- Services, batch jobs, and supported parallel task groups.
- Cancellation, deadlines, retries, and completion.
- Scaling and updates where supported.
- Recovery following node loss or control-plane restarts.

Reject cyclic task dependencies unless a separate execution model explicitly
supports them.

### 5.4 Scheduler

The scheduler chooses eligible placements according to:

- Resource requirements and current reservations.
- Hardware and runtime compatibility.
- Security and tenancy constraints.
- Affinity, anti-affinity, topology, and data locality.
- Priority, fairness, and preemption policy.
- Network requirements and estimated transfer cost.

The scheduler proposes placement. The allocator commits reservations. The
node agent validates and enforces the resulting allocation.

For tightly coupled parallel workloads, support explicit gang-scheduling
semantics rather than accidentally starting an unusable partial group.

### 5.5 Allocator

The allocator owns resource accounting and reservation policy.

It must:

- Prevent concurrent placement decisions from overcommitting non-shareable
  resources.
- Enforce tenant quotas and admission limits.
- Distinguish requests, reservations, limits, and measured usage.
- Support resource sharing only when policy explicitly permits it.
- Reclaim expired or released allocations safely.
- Explain why an allocation was accepted or rejected.

A reservation is not proof that a worker started. Execution requires a
separate acknowledgment from the node agent.

### 5.6 Image creator and loader

Image facilities support reproducible image construction and safe loading.

Responsibilities include:

- OCI image validation and digest verification.
- Platform and architecture selection.
- Layer deduplication and bounded caching.
- Registry authentication.
- Optional signature and provenance verification.
- Safe extraction with path and ownership validation.
- Import and export for disconnected deployments.

Use immutable digests for reproducible execution. Resolve mutable tags to
digests and record the selected digest before starting an attempt.

Treat image builds as untrusted code execution and isolate them accordingly.

### 5.7 Node discovery and membership

Discovery locates candidate peers; membership determines trusted participants.

Support deployment-appropriate discovery through static configuration,
service discovery, local discovery, or explicit bootstrap endpoints.

Discovery alone must never grant cluster membership.

Membership requires authenticated enrollment, authorization, and a persistent
node identity. Track availability separately from identity and trust.

### 5.8 Local and wide-area networking

Networking facilities provide:

- Intra-node communication.
- Cluster-local connectivity.
- Optional ad-hoc peer connectivity.
- Cross-network routing and relay support.
- Connection recovery, flow control, and congestion awareness.

Account for NAT, firewalls, intermittent links, limited bandwidth, and
asymmetric connectivity.

Distinguish control traffic from application data traffic. Bulk artifact
transfers must not starve heartbeats, cancellation, or lease renewal.

Ad-hoc and wide-area connectivity must be explicitly enabled and must not
bypass authentication or network policy.

### 5.9 Message passing and IPC

Expose explicit communication contracts for supported transports:

- Ordering scope.
- Delivery and acknowledgment semantics.
- Message size limits.
- Timeout and cancellation behavior.
- Backpressure and queue limits.
- Authentication and authorization.
- Protocol version negotiation.

Use local IPC where appropriate and authenticated, encrypted transport
between nodes.

Do not promise exactly-once execution or delivery without a narrowly defined,
enforceable protocol. Assume operations may be retried and require
idempotency keys or deduplication where necessary.

Shared memory, accelerator collectives, MPI, and RDMA require explicit
capability checks. Do not silently substitute transports when doing so would
change required application semantics.

### 5.10 Data spool and artifact storage

The spool buffers staged inputs, intermediate results, and pending transfers.

It must provide:

- Content verification.
- Atomic publication of completed artifacts.
- Resumable transfers where supported.
- Disk quotas and configurable retention.
- Backpressure when capacity is exhausted.
- Crash recovery and orphan cleanup.
- Tenant-aware access control.
- Garbage collection that respects active references.

Separate temporary spool data from durable outputs. A successful task must
not imply durable output persistence unless its output policy guarantees it.

## 6. Resource Attributes and Placement Policy

Resource attributes belong primarily to nodes and allocations. Workers also
contribute observed usage and task-specific constraints.

Each measurement should identify its units, source, timestamp, validity
window, and confidence or verification status.

### Compute power

Describe CPU architecture, usable cores, instruction sets, accelerator type,
device memory, and relevant benchmark results.

Do not reduce heterogeneous hardware to a single interchangeable capacity
number.

### Bandwidth and latency

Track bandwidth and latency by relevant path or peer, not only as a
node-wide scalar.

Distinguish advertised capacity, measured throughput, reserved bandwidth, and
recent congestion. Stale measurements must be discounted or rejected.

### Permissions

Permissions are hard authorization constraints, not scheduling preferences.

Examples include permission to access devices, networks, secrets, datasets,
host paths, or particular workload classes.

### Capabilities

Capabilities describe supported functionality, including:

- Runtime and operating-system support.
- GPU drivers and accelerator APIs.
- Checkpoint and restore support.
- Trusted execution or stronger isolation modes.
- Specialized networking and storage.

Self-reported capabilities may require independent verification.

### Eagerness

Eagerness expresses an owner's willingness to accept additional work, such as
on opportunistic or intermittently available nodes.

Define its range and default in the schema. It must not override resource
limits, trust requirements, or admission policy.

### Niceness

Niceness expresses relative scheduling preference within an authorized
priority class.

Use a documented ordering; preferably, larger niceness values mean lower
priority. Tenants must not gain administrative priority by editing niceness.

### OMNI score

The OMNI score is an optional, versioned ranking heuristic for eligible
placements. It is not a security boundary or a universal hardware benchmark.

Compute it only after applying hard constraints.

A scoring policy should document:

- Input metrics and normalization.
- Weights and policy version.
- Treatment of missing or stale observations.
- Workload-specific cost estimates.
- Fairness and starvation controls.
- Stable tie-breaking behavior.

Expose the contributing factors in placement diagnostics. Untrusted reports
must not directly determine scheduling priority.

## 7. Monitoring and Recovery

Use one shared telemetry pipeline with specialized monitors rather than
duplicating health logic across unrelated daemons.

### Worker health monitor

Track liveness, readiness, progress, and execution deadlines independently.

A reachable worker may be unhealthy; an unreachable worker may still be
running. Missing heartbeats indicate suspicion, not proof of termination.

### Worker security monitor

Detect policy violations and suspicious behavior. Support audited responses
such as alerting, isolation, credential revocation, or termination.

Automated responses must be policy-controlled and avoid exposing secrets in
diagnostic output.

### Worker bandwidth monitor

Measure transfer rates, retransmissions, queue pressure, and network budget
consumption.

### Worker resource monitor

Measure CPU, memory, device, storage, process, and file-descriptor usage.
Distinguish transient pressure from sustained exhaustion.

### Worker recovery monitor

The recovery monitor, also called the resuscitation monitor, coordinates:

- Restart of failed attempts.
- Rescheduling after node loss.
- Checkpoint restoration when supported.
- Quarantine of repeatedly failing nodes or artifacts.
- Escalation when retry budgets are exhausted.

Use bounded exponential backoff with jitter. Avoid restart storms and
repeated retries of deterministic failures.

Checkpoint support is runtime- and workload-dependent. Never imply that
arbitrary GPU or multi-node workloads can be transparently migrated.

## 8. The VMM Aspects

Omnibuild can inject a lightweight kernel into the image archive. That kernel is always
Omnix, or any kernel that follows Omnix's behavior. When Omnibuild does this,
it encodes a different magic into the image's first 8 bytes. That's how
Omnirun knows this is a kernel-carrying image.

### Initsys

Whether launched with or without Omnix (the kernel), the container needs an
init system. Initsys is that system. Initsys starts the initial process in
the container, and all the subsequent processes will be a child of Initsys.

Initsys is, as such, the unofficial daemon of a container (regardless of 
being launched within the confines of Omnix or not). We can control much
of the container's behavior with Omnix. Because every process launched
downstream of Omnix has the Omnix Daemon instrumentalized within its
entrypoint.

`initsysctl` is a utility that let's the user control Initsys. It has
commands such as:
- `initsysctl kill [signal] [pattern]`: send a signal to the processes that follow the pattern.
- `initsysctl trace [pattern]`: insert a tracepoint at the processes with that pattern.
- `initsysctl reboot`: reboot (in Omnix mode)
- `initsysctl exit`: exit the container

`initsysctl` has many commands. You can find a full list of them in `manifest/initsysctl-commands.yaml`.

### Frantz

Frantz is a minimal EFI-like firmware that handles the VMM mode. When you launch
an image that has Omnix, Omnirun reads the header of that image. If the header's magic
signals presence of Omnix, it starts the booting sequence and injects Omnix into
the memory.

### Meshbox

Initsys loads Meshbox first when it loads. Meshbox is a collection of POSIX unilities. 
Even if you don't inject Omnix, you can have Omnibuild inject Meshbox and use its
utilities. Meshbox implements all of POSIX utilities, inclduing `sh`.

Unlike the entire system which is implemented in C & C++, Meshbox is implemented in D.

## 8. Lifecycle and Distributed Correctness

Keep desired state separate from observed state.

A typical task lifecycle includes:

- Pending: awaiting dependencies or admission.
- Queued: eligible for placement.
- Allocated: resources reserved.
- Starting: preparing inputs and execution.
- Running: execution acknowledged.
- Succeeded: completion conditions satisfied.
- Failed: no remaining recovery policy applies.
- Cancelled: cancellation finalized.

Retries create new attempt records; they do not overwrite execution history.

Represent uncertain attempt observations explicitly, such as `Unknown`.
Do not declare an attempt stopped solely because its node is unreachable.

Distributed state changes must account for:

- Duplicate and reordered messages.
- Partial failure.
- Agent and controller restarts.
- Clock skew.
- Network partitions.
- Stale leaders and expired leases.

Use a defined consistency mechanism for authoritative reservations and
ownership. Merely running multiple controllers does not provide safe high
availability.

Lease-based ownership needs fencing tokens or an equivalent mechanism to
reject stale operations. Lease expiry alone does not stop an isolated
process from producing external side effects.

Tasks with external side effects require an explicit retry policy and, where
needed, application-level idempotency or fenced writes.

## 9. Workload Configuration

The workload schema should provide versioned fields for:

- Workload identity, tenant, and metadata.
- Image digest, command, arguments, and environment.
- Task graph, replicas, or supported parallel execution model.
- CPU, memory, accelerator, storage, and network requirements.
- Placement constraints and topology preferences.
- Input artifacts, output declarations, and durability policy.
- Secret references and service identity.
- Network and communication requirements.
- Priority, niceness, deadlines, and retry budgets.
- Health checks and termination grace periods.
- Checkpoint, recovery, and partition behavior.
- Security profile and isolation requirements.

Validate schemas before scheduling. Report errors with field paths and
actionable explanations.

Use explicit units and document defaults. Reject unknown fields unless they
belong to a documented extension mechanism.

Secrets must be referenced rather than embedded in workload definitions,
images, logs, or command-line diagnostics.

Provide a validation or dry-run operation that explains admission results
without reserving resources or starting containers.

## 10. Security Requirements

- Authenticate node-to-node and node-to-control-plane communication.
- Authorize API calls and data access by tenant and workload identity.
- Encrypt traffic crossing trust boundaries.
- Support credential rotation and revocation.
- Prefer rootless or minimally privileged execution where supported.
- Apply platform-appropriate filesystem, syscall, and device restrictions.
- Disable privileged containers and host namespace access by default.
- Restrict host mounts and management endpoint exposure.
- Verify artifact integrity before use.
- Protect persisted credentials and sensitive spool data.
- Audit administrative changes and security-sensitive operations.

Container isolation is not equivalent to a virtual machine security
boundary. Deploy mutually untrusted workloads with an isolation mode
appropriate to the threat model.

## 11. Observability and Operations

Provide structured logs, metrics, and traces with correlation identifiers for
workloads, tasks, attempts, allocations, nodes, and requests.

Operational interfaces should support:

- Workload and node inspection.
- Placement explanations and unschedulable-task diagnostics.
- Queue and quota visibility.
- Node cordon and drain.
- Cancellation and controlled retry.
- Artifact and spool inspection.
- Credential rotation.
- Backup and recovery of authoritative state.

Keep metric cardinality bounded. Redact credentials and sensitive payloads.

Document graceful shutdown, upgrade compatibility, storage migration, and
recovery procedures. Draining a node must stop new allocations before
handling existing workers.

## 12. Guidance for Coding Agents and Contributors

Before making changes:

1. Read relevant source files, nearby tests, and scoped contributor guidance.
2. Identify existing interfaces and invariants.
3. Determine whether the feature is implemented, experimental, or planned.
4. Prefer the smallest coherent change that satisfies the task.

While implementing:

- Preserve subsystem boundaries.
- Follow repository language, formatting, and dependency conventions.
- Propagate cancellation and deadlines across blocking operations.
- Bound concurrency, memory usage, queues, and retries.
- Use monotonic time for local elapsed-time measurements.
- Keep security decisions separate from scheduling heuristics.
- Avoid unrelated refactors and speculative abstractions.
- Do not add dependencies without a clear need.
- Do not replace working behavior with stubs or hard-coded success.
- Update schemas, documentation, and compatibility notes together.

Never invent repository paths, commands, benchmark results, or test outcomes.
Use the actual build and test tooling present in the repository.

If architectural intent conflicts with existing behavior, identify the
difference explicitly. Do not silently introduce a breaking change.

## 13. Testing Expectations

Changes should include appropriate coverage for:

- Schema validation and policy evaluation.
- Scheduling constraints and reservation races.
- Resource accounting and quota enforcement.
- OCI lifecycle and image handling.
- Authentication, authorization, and tenant isolation.
- Duplicate, delayed, and reordered messages.
- Network partitions and reconnection.
- Agent crashes and controller failover.
- Disk exhaustion, spool corruption, and interrupted transfers.
- Retry exhaustion, cancellation, and stale ownership.
- Mixed-version compatibility where supported.

Use deterministic clocks, seeded randomness, and controlled fault injection
where practical.

Hardware-dependent tests must declare prerequisites and skip explicitly when
unavailable. Simulated accelerator tests do not establish real-device
compatibility.

Benchmark performance-sensitive changes against a documented baseline.
Do not report performance improvements without measurements.

## 14. Extensibility

OmniMesh provides two primary mechanisms for extending system capabilities: the native C/C++ Plugin ABI/API for high-performance, low-level subsystem integration, and the sandboxed Lua extension system for lightweight, dynamic policy evaluation.

Neither extension mechanism may bypass authentication, admission control, tenant boundaries, or resource quotas.

### 14.1 Native Plugin ABI and API

Native plugins are packaged as dynamically loaded libraries (`.so`) implementing the interfaces declared in `include/OmniMesh-Plugin.h` (pure C ABI) and optionally wrapped by `include/OmniMesh-Plugin.hpp` (idiomatic C++ abstractions).

#### 14.1.1 Extension points

Native plugins extend subsystem functionality without modifying core binaries:

- **Device and hardware discovery:** Custom accelerator discovery, hardware health tracking, and topology interrogation (e.g., custom ASICs, specialized NPUs, or proprietary interconnects).
- **Custom scheduling and scoring:** Pluggable placement filters and custom OMNI scoring evaluators for specialized cluster topologies.
- **OCI runtime hooks:** Lifecycle intercepts invoked before container creation, after bundle assembly, or after worker termination.
- **Data spool and storage drivers:** Content-addressed backends, distributed spool synchronizers, or specialized volume mounts.
- **Telemetry and audit sinks:** Direct streaming of metrics, execution traces, and audit logs to external infrastructure.

#### 14.1.2 ABI stability and safety conventions

- **Strict C ABI boundary:** Public symbol exports must use standard C linkage (`extern "C"`). C++ exceptions must never cross the ABI boundary; plugins must capture exceptions internally and return explicit error codes (`omni_status_t`).
- **Version handshake:** Every plugin must export `omni_plugin_init` and declare compatibility with `OMNIMESH_PLUGIN_API_VERSION`. Mismatched API generations must be rejected immediately during load time.
- **Memory ownership:** Memory allocations crossing the ABI boundary must be freed by the allocating allocator. Core interfaces provide explicit allocator and deallocator function pointers in the plugin context structure.
- **Thread safety:** Plugins must be re-entrant and thread-safe unless an interface explicitly defines single-threaded execution guarantees.
- **Panic and crash isolation:** Plugin operations execute within the host process domain. A fatal signal within a plugin crashes the host daemon; critical production extensions should prefer external helper processes or isolated sidecars when stability cannot be formally guaranteed.

### 14.2 Lua Extension System and `lomesh`

For high-level admission policies, scheduling heuristics, dynamic task mutations, and custom health validation, OmniMesh embeds a sandboxed Lua engine exposing the `lomesh` standard module.

#### 14.2.1 Extension capabilities

The Lua engine provides hooks into:

- **Workload admission and validation:** Declarative inspection and rejection of submitted workload specifications before admission to the queue.
- **Task mutation and defaulting:** Dynamic injection of environment variables, labels, affinity constraints, or spool configurations based on tenant or cluster policy.
- **Custom health and progress checks:** Interpreted evaluation of complex worker heartbeats, progress logs, or output telemetry.
- **Heuristic placement hooks:** User-defined scoring functions evaluated alongside the core scheduler.

#### 14.2.2 The `lomesh` standard module

The embedded runtime exposes the `lomesh` table, providing safe bindings to core data types:

- `lomesh.workload`: Read-only inspection and controlled mutation of task specs.
- `lomesh.node`: Read-only queries of advertised node attributes, topology tags, and available capacities.
- `lomesh.log`: Structured logging routed through the daemon's unified telemetry pipeline with automatic correlation fields.
- `lomesh.crypto`: Content hashing (SHA-256, BLAKE3) and digest validation utilities.

#### 14.2.3 Sandboxing and resource bounds

- **Restricted environment:** Lua scripts execute in a stripped environment. Direct access to host filesystems, arbitrary network sockets, OS system calls, and unsafe standard libraries (`os`, `io`, `debug`, `package`) is removed or replaced with safe proxies.
- **Execution budgets:** Every Lua invocation runs with strict instruction quotas (CPU instruction count limits) and memory limits per state. Scripts exceeding memory budgets or exceeding execution time limits must be interrupted and failed immediately.
- **Side-effect isolation:** Dynamic hooks must be functionally pure and deterministic. Scripts must not retain cross-invocation global state that could alter scheduling correctness.

### 14.3 Extension Lifecycle and Operational Controls

- **Fail-open vs. fail-closed configuration:** Each registered extension (native or script) must specify its failure mode (`FAIL_ABORT`, `FAIL_REJECT`, or `FAIL_IGNORE`). Critical security, admission, and device enforcement hooks must default to `FAIL_REJECT` or `FAIL_ABORT`.
- **Registration and discovery:** Extensions are configured declaratively in node or control-plane manifests. Dynamic runtime loading of untrusted plugins without administrative authorization is prohibited.
- **Observability:** Invocations of native plugins and Lua hooks must emit timing metrics, error counters, and audit traces to identify misbehaving or slow extensions without obscuring core system performance.

## 15. The OmniMesh Toolchain

OmniMesh is shipped with a toolchain. This toolchain includes:
- OmniCC: an ISO C23 compiler with its own libc, using author's own Firestone library
- OmniPP: the C preprocessor
- OmniAS: the assembler, using author's own Sandstone library
- OmniLD: the linker, using author's own Yellowstone library
- OmniFuzz: a Directed Graybox Fuzzer
- OmniLint: a static analyzer
- OmniBench: a benchmarking tool
- OmniCheck: a unit tester
- OmniDGB: a debugger
- OmniTerm: a terminal emulator
- OmniW3: a hybrid TUI/GUI browser
- OmniBuild: a build system
- OmniBus: an inter-process messaging system
- OmniSys: an init system, daemon manager that uses OmniBus
- OmniChron: a chron system
- OmniWM: a window manager (that uses OmniTerm as its primary terminal)
- OmniWin: a windowing system (that OmniWM uses as one of its backends)
- OmniRPC: an RPC system

All these tools are optionally loaded into images by OmniBuild.

## 16. Vendored and Third-Party Libraries

All the third-party libraries are stored in `third_party` directory. These libraries are:

* Authors librarries:
- Sandstone: https://github.com/Chubek/sandstone
- Firestone: https://github.com/Chubek/firestone
- Yellostone: https://github.com/Chubek/yellowstone

* Vendored:
- Asio (standalone version)
- gRPC + Protobuf
- mbedtls
- libcurl
- nlohmann/json
- zstd
- zlib
- libarchive
- {fmt}
- spdlog
- Catch2
- crun
- cgroups
- libseccomp
- libcap
- libsystemd
- libselinux
- libapparmor
- hwloc
- libnuma
- libudev
- libevdev
- Nvidia NVML
- AMD SMI
- lmdbxx
- libsql
- etcd
- liburing
- Skopeo
- buildah
- libmnl
- c-ares
- msquic
- libnice
- ZeroMQ
- nng
- libbpf
- OpenTelemtry C++
- CRIU
- CLI11
- yaml-cpp
- TOML++
- valijson
- libfuzzer
- libproc2
- unibilium
- termbox2
- libvterm
- libtermkey

All these libraries are availble under `third_party/` directory. We use a mix of CMake and Ninja as the build system for OmniMesh. 

## 17. Definition of Done

A change is complete when:

- Its behavior and failure modes are defined.
- Relevant tests pass, or unrun tests are clearly identified.
- Resource cleanup and restart behavior are accounted for.
- Security and tenancy implications have been considered.
- Public interfaces and schemas are documented.
- Compatibility and migration requirements are stated.
- Logs and diagnostics make failures actionable.
- No unsupported capability is presented as implemented.

Contributor summaries should state what changed, what was validated, and any
remaining limitations.

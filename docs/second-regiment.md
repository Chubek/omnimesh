# Second implementation regiment

The second regiment adds an experimental foreground execution path to the first
regiment's control-plane planner. It operates on one local Linux node, a trusted
administrator-provisioned rootfs, and an explicitly selected OCI runtime binary.
It also repairs and exercises the separately introduced control-plane journal.
No OCI conformance or real-runtime compatibility has been established by the
protocol fixture tests.

## Execution interface

```sh
build/omnimesh-node-agent run manifests/example-workload.json \
  --node manifests/example-node.json \
  --runtime /usr/bin/crun \
  --rootfs /absolute/path/to/provisioned-rootfs \
  --state-dir /absolute/path/to/fresh-session \
  --timeout-ms 60000 --grace-ms 1000
```

The runtime is an administrator-installed executable. OmniMesh does not download
or build crun/runc. The rootfs must already contain the workload executable and
its dependencies, plus mount points for `/proc`, `/dev` and `/tmp`. The rootfs
path must be outside the private session directory and must not resolve to `/`.
The administrator must provision the correct image and verify its digest before
execution: the image field is recorded as an annotation, and does not authenticate
rootfs contents. Image loading, registry access and layer verification are planned.

The node document is administrator-supplied inventory. Its architecture must
match the local host (`amd64` or `arm64`), and normal readiness, verification,
tenant, resource and label placement filters still apply. Node discovery and
authenticated membership are planned. Local execution rejects optional
capability requirements and workloads whose replicas multiplied by maximum
attempts exceed 64. One worker is active at a time; other replicas queue.

The session deadline defaults to 60000 ms and accepts 1–3600000 ms. Termination
grace defaults to 1000 ms and accepts 0–60000 ms. The deadline includes retries
and their backoff. Shutdown and verification can extend elapsed time past this
deadline: each runtime control command is bounded by two seconds, followed by
bounded child reaping, and forced-stop verification gets an additional three
seconds. The deadline is a cancellation trigger, not an instantaneous return
promise.

Successful execution returns zero. Invalid input returns two. Placement denial,
cancellation, failed tasks, uncertain termination and failed runtime cleanup
return three. Internal failures return one. JSON output includes task states,
attempt history, container IDs, exit codes, cleanup statuses, and retained
reservation status. Each worker captures the first 64 KiB of stdout and stderr;
excess bytes are drained and counted. Invalid UTF-8 output is replaced when
rendering JSON. Application output is not automatically redacted.

## Runtime and isolation contract

`prepare_bundle` emits an OCI Runtime Specification 1.0.2 configuration with a
read-only rootfs, separate PID/network/IPC/UTS/mount/user/cgroup namespaces, an
empty capability set, `noNewPrivileges`, CPU and memory limits, a 256-process
limit, and bounded tmpfs mounts. Container UID/GID zero map only to the invoking
host UID/GID. There are no host mounts, external networking or device adapters.
A syscall filter is not implemented. This experimental profile is not a claimed
security boundary for mutually untrusted tenants.

The runtime must implement the runc/crun command protocol used by the adapter:
`--root ROOT run --keep --bundle BUNDLE ID`, `state ID`,
`kill --all ID TERM/KILL`, and `delete ID`. Attached `run` must wait for the
container's initial process and propagate its exit code. State must identify the
requested container; malformed, duplicate-key, excessive or unknown state JSON
fails verification. Runtime state queries acknowledge `Running`; reservations
alone do not.

Cancellation through SIGINT, SIGTERM or the session deadline first cancels
controller desired state, then signals all container processes with TERM and
escalates to KILL after grace. A matching runtime `stopped` state is required
before reporting a terminal attempt or releasing its reservation. An exited
runtime monitor alone is insufficient. Normal nonzero exits use the declared
retry budget; cancellation and spawn failures do not retry. Each retry creates a
new attempt and bundle. Cleanup uses non-forced delete only after confirmed stop.

## Local authority and manual recovery

Only one session per host UID can hold the advisory lock at
`/tmp/omnimesh-local-agent-UID.lock`. Before invoking a runtime, the agent writes
and syncs the session path into this file. Confirmed termination of all allocated
attempts clears it. An uncertain result or process crash leaves the barrier in
place and prevents another session for that UID, even with a new session path.
Existing session directories are never reused automatically.

For recovery, inspect the session named in the authority file and the runtime's
state under `SESSION/runtime`. Use the same runtime binary and root to inspect
and terminate every container, confirm it stopped, and delete its runtime state.
Keep the authority file locked while performing recovery and clear its contents
only after all containers are confirmed stopped. Retain session files for
diagnostics or remove them according to an administrator's retention policy.
Do not unlink the authority file while another process might hold it.

This barrier survives process death while `/tmp` survives. It is not a durable
reservation database, does not cover separate host users or separate `/tmp`
mounts, and does not provide distributed fencing. It cannot by itself prove
that an interrupted task's external side effects may safely be retried. Manual
recovery must account for those effects. Containers may outlive an agent killed
with SIGKILL; new work must not start by bypassing the barrier.

## Experimental journal

`Journal` stores version-one little-endian frames with a sequence, record type,
payload length and noncryptographic checksum. A private file lock excludes
concurrent writers. Payloads are bounded to 64 KiB, logs to 16 MiB and record
counts to 65536. Appends sync before acknowledgment and recovery verifies the
entire framing before applying callbacks. Only incomplete final frames or
payloads are truncated; complete checksum, version and ordering errors preserve
the file and fail recovery. Compaction and storage migration are not implemented.

`DurableControlPlane` validates and records node, quota, desired-workload,
reservation, cancellation and observation events. Recovered active attempts are
`Unknown`, with accounting retained until fresh runtime observations arrive.
Historic `Running` events cannot establish current liveness. Recovery rejects
unknown or invalid records rather than dropping ownership facts. A failed
recovery may leave partially reconstructed in-memory state; callers must discard
it and must not schedule against it.

The journal is a manually invoked library interface. It does not transactionally
wrap live allocator/controller mutations and is not used by `plan` or the local
node-agent CLI. Callers must record facts before external operations. There are
no leases, controller failover or distributed fencing tokens.

## Validation

`process`, `execution` and `agent-cli` exercise actual subprocesses using a
compiled protocol fixture. They cover literal argument handling, environment
and descriptor isolation, output flooding, retries, failed cleanup, cancellation,
TERM-to-KILL escalation, failed state verification, command timeouts, retained
reservations, and the recovery barrier after an agent is killed. The fixture
provides no container isolation and establishes no OCI conformance.

`storage` covers bounded appends, reopen-before-replay, exclusive ownership,
incomplete writes, corruption rejection, manifest encoding, reservation recovery,
retry history and refusal to restart uncertain recovered attempts.

The optional `oci-runtime` test skips with code 77 unless both variables below
are set. It requires an administrator-provisioned rootfs with `/bin/true`, a
runtime supporting this profile, Linux user namespaces, and cgroup delegation
sufficient for CPU, memory and process enforcement. An enabled test fails if
those capabilities are unavailable; it does not silently omit limits.

```sh
OMNIMESH_TEST_OCI_RUNTIME=/usr/bin/crun \
OMNIMESH_TEST_OCI_ROOTFS=/absolute/path/to/test-rootfs \
ctest --test-dir build -R '^oci-runtime$' --output-on-failure
```

Real runtime and hardware compatibility remain unverified when this test skips.
Distributed transports, artifacts, image loading, automated recovery, monitoring,
plugins, Lua, VMM, Omnix, Initsys, Meshbox and Frantz remain later work.

Validation on October 5, 2026: the GCC Debug suite passed ten tests and skipped
the optional real OCI test. The Clang AddressSanitizer, UndefinedBehaviorSanitizer
and LeakSanitizer suite produced the same result. LeakSanitizer required running
outside the process-inspection-restricted sandbox. Targeted clang-tidy analyzer
checks reported no diagnostics in the worker, agent, runtime, journal and event
recovery implementations. No real OCI runtime was available for validation.

## Compatibility

The workload and result API remain `omnimesh.io/v1alpha1`. The journal's byte
framing remains version one, including its original checksum seed. Invalid
event envelopes from the unfinished implementation are rejected and preserved;
there is no automatic migration for such files. Builds explicitly require Linux
and disable local execution if safe spawn descriptor isolation is unavailable.
Recovered reservations now become Unknown instead of Allocated; callers must
obtain fresh runtime observations before treating those attempts as running or
stopped. Existing session directories remain available for inspection and are
never automatically reused.

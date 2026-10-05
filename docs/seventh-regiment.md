# Seventh implementation regiment

The seventh regiment connects the local content-addressed artifact spool to
workload execution. The foreground Linux node agent verifies declared input
blobs and prepares a bounded session snapshot before reserving resources or
starting an attempt. This remains an experimental local execution profile;
real OCI isolation and mount enforcement require validation with a configured
runtime. There is no OCI conformance claim.

## Declare and publish inputs

An optional `spec.inputs` array contains at most 32 objects, each with exactly
`name` and `digest`. Names use the workload identifier syntax: 1-63 lowercase
letters, digits or internal hyphens, starting and ending with a letter or digit.
Names must be unique. Digests must be `sha256:<64 lowercase hex>`; repository
prefixes and mutable references are rejected. No arbitrary destination, host
path, writable mount or extraction settings are accepted.

For example, publish the three bytes `abc` under the workload's tenant:

```sh
printf abc > input.bin
build/omnimesh artifact put input.bin \
  --spool-dir /absolute/path/to/spool --tenant local
```

Add these fields to the workload's `spec`:

```json
{
  "command": ["/bin/cat", "/tmp/omnimesh-inputs/dataset"],
  "inputs": [{
    "name": "dataset",
    "digest": "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
  }]
}
```

Then execute with either a provisioned rootfs or a verified local OCI layout:

```sh
build/omnimesh-node-agent run workload.json \
  --node manifests/example-node.json --runtime /usr/bin/crun \
  --image-layout /absolute/path/to/layout \
  --state-dir /absolute/path/to/fresh-session \
  --spool-dir /absolute/path/to/spool \
  --max-input-bytes 268435456 \
  --journal-dir /absolute/path/to/journal
```

The spool must already exist and be private to the host caller. The fresh
session must be outside the spool, image layout and provisioned rootfs.
`--spool-dir` is required exactly when the workload declares nonempty inputs.
`--max-input-bytes` requires `--spool-dir`, accepts 1 through 268435456 bytes,
and defaults to 256 MiB. The agent opens the spool with its existing default
capacity/count options. Each artifact remains bounded at 64 MiB. Empty blobs
are supported. Multiple names may reference one digest; each copy counts
toward the aggregate session limit.

Validation and placement dry runs check the input schema without opening the
spool, confirming availability, staging files or reserving resources.

## Preparation and worker access

When using an image layout, image verification completes first. The agent
opens the spool under its exclusive directory lock, enforces the workload
tenant label and re-hashes each blob while copying in 64 KiB chunks. Output
is published only after indexed size and SHA-256 verification. Fetches reject
nonregular blobs without waiting for a FIFO writer. A fetch holds the spool's
mutex through verification and publication, preventing concurrent collection
or close from invalidating that operation.

Completed files reside at `SESSION/inputs/<name>` with mode `0400`; the completed
directory has mode `0500`. Every replica and retry uses the same snapshot.
The bundle requires the exact declared names, owned regular files without
symlinks or hardlinks, and protected modes. It adds a nonrecursive bind mount
with `bind,ro,nosuid,nodev,noexec` at `/tmp/omnimesh-inputs`, after the private
`/tmp` tmpfs mount. No executable input permission is granted. The spool
directory and its index are never mounted into a worker.

The snapshot is an independent copy, so later spool garbage collection cannot
remove the bytes already prepared for the session. The agent releases the
spool lock before any attempt. Spool quotas and session staging quotas are
separate; the session's retained disk usage is not charged to the spool.

The monotonic whole-session deadline includes spool opening, reconciliation
and copying. Cancellation is checked between directory entries and streaming
chunks, and before publication. Filesystem calls and bounded JSON parsing
remain synchronous, so deadlines are cooperative. C++ cancellation callbacks
for `ArtifactSpool::open` and `FetchOptions::cancelled` must not throw or
re-enter the spool while its mutex is held. `FetchOptions::max_bytes` adds a
per-fetch bound without weakening the 64 MiB limit. Destruction releases the
spool lock; callers use `close()` when they need its explicit persistence status.

## Failures, results and recovery

Missing blobs, mismatched labels, corrupt bytes, size limits, cancellation,
spool lock contention and filesystem errors fail before allocation or launch.
Preparation failure cancels desired state and consumes no attempt or retry
budget. With a journal, cancellation is recorded before mutation; a recording
failure fails closed. Validation failures can occur earlier without a session
or cancellation record. The runtime authority barrier is marked only at start.

Failed fetches remove their temporary copy and preserve stored blobs and any
previous output. Earlier completed input files and image files are retained
for inspection. Failed sessions are never reused. An index rebuild discards
tenant labels as before; unlabeled inputs cannot be fetched until the trusted
administrator republishes them for the intended tenant.

`LocalExecutionResult` adds an `inputs` object, for example:

```json
{
  "path": "/absolute/path/to/session/inputs",
  "verified": true,
  "files": [{
    "name": "dataset",
    "digest": "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    "bytes": 3,
    "destination": "/tmp/omnimesh-inputs/dataset"
  }]
}
```

`verified` means the entire input preparation completed. It is false for an
input-free workload or incomplete preparation. Failed preparation may list
earlier completed files, but the incomplete snapshot is unusable. Verification
does not protect against subsequent hostile changes by the trusted host user.
Direct C++ callers of `prepare_bundle` must supply a previously digest-verified
snapshot; that function validates its structure and mounts, without re-hashing.

Journaled workload records preserve the input names and digests. Recovery
replays facts and cancellation without opening the spool, trusting old
snapshots or restarting work. Existing uncertain-execution recovery rules
remain in force. Successful and failed sessions retain their files;
administrators manage retention across sessions. To remove a completed input
snapshot, first restore owner write permission on its directory.

## Compatibility and boundaries

The workload API remains `omnimesh.io/v1alpha1`. Empty or omitted inputs preserve
existing execution behavior. Older strict decoders reject nonempty `inputs`,
including journals containing those workload records; upgrade consumers before
using the field, and do not downgrade those journals. Journal framing and spool
format are unchanged. C++ consumers must rebuild against the extended structs
and method signatures.

Tenant labels remain discretionary checks within a trusted local administrative
profile. They do not authenticate a submitting user or isolate mutually
untrusted tenants. Registry access, network artifact transfer, arbitrary mounts,
input archive extraction, output collection, durable output policy and automatic
session retention remain unimplemented.

## Validation

The `manifest` suite checks strict fields, names, digests, duplicates, count
bounds, equality and serialization round trips. `artifacts` checks streaming
cancellation, byte bounds, output preservation, FIFO rejection, cancelled
reconciliation and lock cleanup. `execution` checks replicas and retries,
snapshot modes, independent copies after collection, missing and unauthorized
inputs, aggregate limits, mid-copy cancellation, corrupt blobs and journal
replay with zero reservations after failed preparation. `agent-cli` combines a
verified OCI image with published spool inputs and checks JSON reporting,
configuration errors and pre-launch quota rejection. The protocol fixture
reads the mount's source and checks requested flags; it does not establish real
kernel mount enforcement.

Validation on October 5, 2026: GCC Debug and Clang AddressSanitizer plus
UndefinedBehaviorSanitizer each passed all 12 runnable CTest suites. Both
skipped `oci-runtime` because no real runtime/rootfs was configured; neither
`crun` nor `runc` was installed. LeakSanitizer required the sanitizer test run
outside the ptrace-based execution sandbox. Targeted clang-tidy analyzer checks
on the agent, spool and bundle implementation reported no diagnostics.
`git diff --check` passed. Fresh build directories were used under `/tmp`.

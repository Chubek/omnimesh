# Third implementation regiment

The third regiment wires the second regiment's experimental control-plane
journal into the live command paths. `plan` and the local node agent can
record the facts they act on, and `omnimesh recover` replays a journal into
fresh memory for inspection. No OCI conformance or real-runtime compatibility
has been established beyond the protocol fixture tests.

## Durable execution interface

```sh
omnimesh plan manifests/example-workload.json \
  --node manifests/example-node.json --journal-dir /absolute/path/to/journal

build/omnimesh-node-agent run manifests/example-workload.json \
  --node manifests/example-node.json \
  --runtime /usr/bin/crun \
  --rootfs /absolute/path/to/provisioned-rootfs \
  --state-dir /absolute/path/to/fresh-session \
  --journal-dir /absolute/path/to/journal

omnimesh recover --journal-dir /absolute/path/to/journal
```

The journal directory is created private (`0700`) and owned by the invoking
user when missing; an existing directory must already be private and owned,
and its `control-plane.journal` file must be a private regular file, or the
command fails before mutating any state. One writer owns a journal through a
file lock. The journal directory must differ from the agent's session
directory; anything else is rejected as invalid input.

`plan` remains a dry run: it records quota, node inventory, desired workload
and the dry run's own reservations, then reports `journaled: true` with the
record count. `recover` prints a `RecoveryReport` with applied, recovered and
skipped record counts plus diagnostics, and exits zero only when every record
replayed. Recovered active attempts are `Unknown`: historic observations
never prove current liveness, and recovery never restarts work or authorizes
execution. A failed recovery discards its partial in-memory state with the
local objects; callers must not schedule against it.

## Recording order and failure mode

Facts are recorded before the mutation or external operation they describe:
node inventory before registration, quota before it is set, workload before
submission, cancellation before the controller cancels, reservations before
any start intent or runtime use for the new attempt, and observations before
they are applied to the controller. Every append syncs before acknowledgment.
Any recording failure cancels desired state where already submitted and ends
the command fail-closed with the journal error; the reported result carries
`journaled: true` and the count of records acknowledged so far.

Reservations are committed inside `reconcile`, under the controller lock, and
are therefore recorded after the commit but before any start intent or
runtime use. A crash between the commit and the record loses the fact, and a
recorded observation whose controller application fails can fail a later
recovery closed. Recovery restores only what was recorded and never assumes
completeness. The journal still does not wrap allocator/controller mutations
transactionally inside their locks, provides no compaction or migration, and
offers no leases, failover or distributed fencing. The local authority lock
and manual recovery barrier from the second regiment are unchanged.

## Validation

`execution` gains a journaled round trip: two replicas execute with a journal
directory, the result reports the record count, and a fresh control plane
recovers two reservations with terminal observations and no retained
accounting. An unusable journal directory fails before any mutation or worker
starts, and a journal directory equal to the session directory is rejected.

`cli` covers journaled planning, `recover` replay with two recovered
reservations, usage errors, and a corrupt journal that is preserved and
reported instead of repaired. `agent-cli` covers a journaled fixture run
followed by `recover`. The library-level journal framing, corruption and
ordering tests from the second regiment are unchanged.

Validation on October 5, 2026: the GCC Debug suite passed ten tests and
skipped the optional real OCI test. The Clang AddressSanitizer and
UndefinedBehaviorSanitizer suite produced the same result. Targeted
clang-tidy analyzer checks reported no diagnostics in the new recording
paths. No real OCI runtime was available for validation.

## Compatibility

The workload and result API remain `omnimesh.io/v1alpha1`. The journal's byte
framing remains version one, including its original checksum seed. `plan`
output gains `journaled` and `journalRecords`; the local execution result
gains the same two fields. `recover` is a new command with JSON output and
the standard exit codes: zero on successful recovery, two for invalid input
or usage, three when recovery fails closed, one for internal failures.
Journals written by earlier builds replay unchanged; invalid event envelopes
are still rejected and preserved with no automatic migration.

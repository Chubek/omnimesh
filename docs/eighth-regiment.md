# Eighth regiment: crash-atomic journal compaction

This describes the eighth-regiment feature. The
[ninth regiment](ninth-regiment.md) supersedes its lock handoff and replacement
verification behavior with continuous ownership and strict verification.

The control-plane journal was append-only with a hard 16 MiB bound. Once a
long-running control plane wrote that many records, every further `append`
failed with `resource_exhausted` and the message
`journal capacity reached; compaction is unsupported`. The control plane could
no longer record reservations or observations, so it would have to stop acting
correctly rather than run out of durable capacity. Compaction reclaims obsolete
records; the complete recovered state must still fit the configured bounds.

## `Journal::compact`

`compact(records)` rewrites a journal so it contains exactly `records`. The
replacement is built, checksummed, fsynced and **re-read** in a sibling
temporary file before the original is touched at all. The swap is a single
`rename(2)` over the journal path, followed by a directory fsync.

Interruption leaves either the original or the complete replacement authoritative:

| Failure point | Result |
| --- | --- |
| Writing the replacement | Original untouched; temporary removed |
| Verifying the replacement | Original untouched; temporary removed |
| Before `rename` | Original untouched; temporary may remain for cleanup |
| During `rename` | Atomic: original or replacement, never neither |
| After `rename`, before directory fsync | Replacement visible; durability of the rename is not yet acknowledged |
| After directory fsync | Replacement durable |

`rename(2)` replaces the directory entry in one step, so a concurrent `open`
never observes a missing journal and cannot create an empty one over committed
state. This is why a plain rename is used rather than an unlink-then-rename
sequence, which would leave exactly such a window.

The eighth implementation dropped the writer lock while swapping and reopening
the journal, requiring writers to be quiesced. The ninth implementation holds
both inode locks throughout publication and transfers the installed handle
directly. Compaction remains an explicit maintenance operation, and callers
must serialize live state mutations while constructing the snapshot.

## `DurableControlPlane::compact`

This derives the complete fact set from live `Allocator` and
`WorkloadController` state and hands it to the journal. Records are emitted in
recovery-application order:

1. `tenant_quota` for every tenant
2. `node_inventory` for every registered node
3. For each workload: `workload_desired`, then `workload_cancelled` if cancelled
4. For each task attempt: `reservation`, then `attempt_observed` if the attempt
   reached a terminal state
5. For each resolved task: `task_resolved`

Attempt history is preserved in order rather than summarized. Replay
re-reserves transiently and the terminal observation releases it again, so
accounting is unchanged and no attempt is lost. Terminal tasks need the
`task_resolved` record because `adopt_reservation` only accepts a task that is
not already resolved.

### Refusing to compact

Compaction validates before it writes:

- **Allocation accounting must agree with attempt history.** If the number of
  active allocations differs from the number of unfinished attempts, the count
  of compacted records would not match the accounting a recovery would build,
  so compaction fails with `conflict` rather than dropping committed capacity.
- **A resolved task must hold no unfinished attempt**, for the same reason.
- **The record set must fit in 16 MiB** after rewriting, checked before any
  file is created.

### What compaction does not claim

An unfinished attempt is compacted as a `reservation` record, which
`adopt_reservation` restores as `Unknown`. A recorded liveness event cannot
establish present liveness, so compaction must not imply a worker is running.
After compaction, `begin_start` on such an attempt returns `conflict`; only a
fresh runtime observation can advance it.

`task_resolved` restores `Succeeded`, `Failed` and `Cancelled` only. Attempt
history that compaction does not need for current state — for example
observation events superseded by later ones — is not retained. Compaction is a
state-restore mechanism, not an audit log.

## Related correction: journaled plans accumulate

While testing compaction, repeated `plan --journal-dir` runs were found to
produce a journal that could not be replayed. Each run reserved against a fresh
empty allocator and then recorded its own reservations, so the journal
accumulated reservations the node's capacity could never satisfy, and recovery
failed closed on replay.

`plan` now recovers the journal before planning and refuses to plan against a
journal that cannot be replayed. Repeated runs accumulate against committed
capacity: once the node is full, later plans report `Queued` with the rejection
reasons and exit `3` without journaling an unsatisfiable reservation. This was a
pre-existing defect, not introduced by compaction, but compaction is unusable
without it because compaction requires a successful recovery.

## CLI

```sh
omnimesh compact --journal-dir DIR
```

Recovers, compacts, and reports `recordsBefore`, `recordsAfter`, `bytesBefore`,
`bytesAfter` and `reclaimedBytes`. It refuses to compact a journal that fails
recovery, and a corrupt journal is reported and left byte-for-byte unchanged.

## Tests

`storage` covers the refusal cases (oversized set, accounting disagreement), the
lock being retained across compaction, that no temporary file survives, that
appends continue from the compacted sequence, that accounting and attempt
history are byte-identical after a compact/recover round trip, that a compacted
`Unknown` attempt cannot be started, and that a resolved task round-trips as
`Succeeded`. `cli` covers compaction through the built binary, including
recovery after compaction and preservation of a corrupt journal.

## Remaining limitations

Compaction has no generation-based schema migration for old journals, and the
complete snapshot must fit the existing state and journal limits. The ninth
regiment fixes the concurrent-opener race; see its contract for failure handling
and cleanup. There are still no distributed leases, fencing tokens or controller
failover.

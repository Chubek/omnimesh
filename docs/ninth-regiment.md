# Ninth regiment: continuous journal ownership and fault recovery

The eighth regiment introduced journal compaction. Its lock handoff closed the
original before rename and reopened the replacement afterwards, allowing another
writer to acquire ownership during publication. This regiment removes that gap
and makes verification and I/O-failure behavior explicit.

## Ownership protocol

`Journal::compact(records)` retains the original journal's exclusive, nonblocking
`flock` throughout preparation. It creates a private, unique sibling file named
`JOURNAL.compact-XXXXXX` using exclusive creation, locks that inode, writes and
syncs its records, then verifies it through the same open stream.

Verification is strict: it never truncates a torn replacement into a valid
prefix. Every frame must pass format, checksum and sequence checks; final record
count and byte count must match the requested snapshot. Empty payloads, oversized
payloads, excessive record counts and excessive total size are refused before
creating a temporary file. A valid empty snapshot is supported.

At rename, **both inodes remain locked**. After rename, the already locked
replacement stream becomes the journal's active handle. The parent directory is
synced before the retired stream is closed. There is no close/reopen ownership
window. Contending commands return `conflict`, including at either side of rename.

A process can open the original inode and be descheduled before calling `flock`.
By the time it resumes, compaction may have retired that inode and released its
lock. `Journal::open` therefore checks the pathname's device/inode against its
descriptor **after acquiring the lock**. A stale descriptor is closed with a
retryable conflict before any initialization, tail repair or write.

This protocol coordinates cooperating processes on the supported local Linux
filesystem profile. Methods on one `Journal` instance require external
serialization. `DurableControlPlane::compact` still requires its caller to
serialize allocator/controller mutations while deriving the complete fact set;
file locking does not provide an atomic cross-object snapshot.

## Failure contract

| Boundary | Authoritative state and permitted next action |
| --- | --- |
| Invalid snapshot | Original unchanged; correct the request and retry. |
| Original sync or strict scan fails | Original preserved, handle faulted; close and recover. Corruption is never compacted away. |
| Temporary creation, write, sync or verification fails | Original unchanged and locked; temporary removed on normal return; retry after addressing the failure. |
| Rename fails | Original unchanged and locked; temporary removed on normal return. |
| Rename succeeds, directory sync or retired-stream close fails | Installed replacement remains locked, but the handle is faulted. Append, replay and compaction return `unavailable`; close and recover. |
| Successful compaction | Replacement durable, still exclusively owned; appends continue after its final sequence number. |

Reopening verifies the journal and syncs its directory even if the file already
exists. This re-establishes directory durability after an uncertain publication
before acknowledging recovery access. Complete corruption remains an error;
ordinary recovery can still repair an incomplete final append according to the
existing journal contract.

Process death before rename leaves the original authoritative. Death after
rename exposes the complete replacement on the running system. A power failure
before directory fsync has unacknowledged rename durability; guarantees depend
on the filesystem's rename/fsync semantics. No power-loss or network-filesystem
conformance is claimed by the process-death tests.

## Bounds, cleanup and operations

The existing limits remain: 64 KiB per record, 65,536 records and 16 MiB per
journal file. A compaction holds one additional file of at most 16 MiB and does
not make an oversized live state fit. The caller supplies the bounded record
vector; no automatic compaction or background controller is introduced.

Normal success and handled failure remove the temporary name. Abrupt death
before rename can leave a private `JOURNAL.compact-*` file. Recovery never
promotes or reads these files. Repeated interrupted compactions can accumulate
orphans: after stopping journal users, an administrator may remove these
temporary files (and the eighth implementation's `JOURNAL.compact` leftover),
preserving `JOURNAL` itself. Successful future compactions use a fresh name and
do not overwrite orphan contents. Automatic orphan garbage collection remains
unimplemented.

The command remains:

```sh
omnimesh compact --journal-dir DIR
```

It must acquire journal ownership first, like `plan` and `recover`. It can be
invoked while another process attempts to open the same journal; one receives a
conflict. It cannot compact a journal held by a running node agent. For a failed
publication, close the owner and run `omnimesh recover --journal-dir DIR` before
planning or execution. Recovery restores unfinished attempts as `Unknown` and
does not authorize restarting them.

## Compatibility

The journal version, record framing, checksums, event schemas and CLI JSON fields
are unchanged from the eighth regiment. Existing valid journals require no
migration. All writers sharing a journal must use the ninth ownership protocol;
stop older binaries before upgrading because they lack the stale-descriptor
check and continuous-lock handoff. No new dependency or public method is added.
Native plugins, distributed leases, fencing and transactional in-lock
control-plane mutations remain outside this implementation.

## Validation

`journal-faults` uses test-only link-time syscall wrappers, with no production
fault-injection API, to exercise:

- Separate-process open attempts immediately before and after rename.
- A precisely delayed opener attempting to lock an inode retired by compaction.
- Rename failure, replacement sync failure and original sync failure.
- Truncated replacement payloads and missing whole frames.
- Directory sync failure after rename, retained ownership and mandatory recovery.
- Corruption detected on an already-open original journal.
- Abrupt process exit before rename, after rename and after successful compaction.

The existing `storage` and `cli` suites cover recovered state, attempt history,
accounting, bounds, continued appends and the compaction command. Real OCI
runtime verification remains a separate optional suite.

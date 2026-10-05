# Fourth implementation regiment

The fourth regiment adds a content-addressed local artifact spool to the
execution plane. `ArtifactSpool` publishes immutable blobs identified by
SHA-256 digests, verifies them on every fetch, enforces tenant labels and
quotas, and rebuilds its index from disk on open. No registry access, image
loading, distributed transfer or worker input wiring has been established.

## Spool interface

```sh
omnimesh artifact put FILE --spool-dir /absolute/path/to/spool --tenant local
omnimesh artifact get sha256:<64 hex> --spool-dir /absolute/path/to/spool \
  --tenant local --out /absolute/path/to/output
omnimesh artifact list --spool-dir /absolute/path/to/spool
omnimesh artifact gc --spool-dir /absolute/path/to/spool [--keep DIGEST ...]
```

Digests are strict `sha256:<64 lowercase hex>`, matching image references.
Only validated digests ever touch the filesystem, so blob names cannot
traverse directories. The spool directory is created private (`0700`) and
owned by the invoking user when missing; anything else fails before any state
changes. One open spool holds an exclusive directory lock against other
processes.

`put` hashes while streaming the source into staging and publishes atomically
with a synced rename; blobs become read-only after publish. Single blobs are
bounded to 64 MiB and the spool defaults to 256 MiB across at most 1024
blobs, rejected with `resource_exhausted` past quota. `get` re-hashes while
streaming into a staged output and verifies before rename; a mismatch removes
only the staged copy, preserves the stored blob, and fails closed. `gc`
removes every blob outside the keep set and reports reclaimed bytes.

## Integrity and recovery model

A blob's name is its content hash, but the name alone is never trusted:
`fetch` always re-hashes, and opening with a lost index re-hashes every blob
before serving anything. A blob that fails verification during open or fetch
fails the operation and is preserved for inspection, never served or deleted.

Tenant labels are recorded at publish time and enforced on fetch; this is
local discretionary labeling, not authenticated isolation. Caller identities
remain trusted input from a future authenticated adapter.

Opening reconciles the directory: interrupted staging files are removed,
blobs missing from the index are dropped, unknown files in the store fail the
open, and an unreadable index is rebuilt by re-hashing, which resets tenant
labels to empty until the bytes are re-published. Rebuilt indexes are noted
in diagnostics, also reported by `artifact list`. There is no compaction
beyond `gc`, no replication, and no distribution beyond this host.

SHA-256 is implemented in-tree (`src/common/sha256.cpp`) because no
cryptographic library is vendored. `finish()` finalizes on a copy, so
intermediate digests never disturb further updates.

## Explicitly out of scope

Workload manifests declare no artifact inputs: no schema, admission or agent
wiring stages spool blobs into worker bundles. Fetching for workers,
registry-backed image loading, digest verification of provisioned rootfs
contents, and cross-node transfer remain later work. The spool does not back
the control-plane journal.

## Validation

`artifacts` covers SHA-256 against system-computed vectors including
multi-block and incremental updates, put/get round trips for files and bytes,
tampered blobs rejected at open and at fetch with the stored blob preserved,
quota and size-bound enforcement, tenant denial, garbage collection, index
rebuild with tenant reset and re-labeling, and foreign files failing the open.

`cli` covers `put` with digest capture, `list`, verified `get` with
byte-identical output, cross-tenant denial, keep-preserving and
collecting `gc`, and usage errors.

Validation on October 5, 2026: the GCC Debug suite passed eleven tests and
skipped the optional real OCI test. The Clang AddressSanitizer and
UndefinedBehaviorSanitizer suite produced the same result. Targeted
clang-tidy analyzer checks reported no new diagnostics in the spool and hash
implementations. No real OCI runtime was available for validation.

## Compatibility

The workload and result API remain `omnimesh.io/v1alpha1`. Artifact digests
use the existing `sha256:<64 hex>` convention. `artifact` is a new command
family with JSON output and the standard exit codes. The on-disk spool layout
(`blobs/`, `tmp/`, `index.json`) is new in this regiment and has no migration
support; foreign files in the store fail the open rather than being adopted.

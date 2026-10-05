# Sixth implementation regiment

The sixth regiment connects local OCI image verification to the foreground
node agent. A workload can now run from a digest-pinned layout without a
separate manual unpack. Image preparation completes before any reservation,
start intent or runtime command. This remains an experimental Linux local
execution profile, with no OCI conformance claim.

## Execute a verified image

```sh
build/omnimesh-node-agent run workload.json \
  --node manifests/example-node.json --runtime /usr/bin/crun \
  --image-layout /absolute/path/to/layout \
  --state-dir /absolute/path/to/fresh-session \
  --max-image-bytes 1073741824 \
  --journal-dir /absolute/path/to/journal
```

`spec.image` must identify the **selected image manifest**, either as
`sha256:<64 lowercase hex>` or `repository@sha256:<64 lowercase hex>`. An
index digest is not a manifest digest. Repository names are descriptive;
the agent does not contact a registry. The node's validated platform selects
the image, and its architecture must match the host. A pin searches all
matching-platform descriptors, so multiple images of the same platform can
coexist in a layout.

Exactly one of `--image-layout` and `--rootfs` is required. Both paths must
be absolute existing directories other than `/`. The fresh session must be
outside the source directory. `--rootfs` retains the administrator-provisioned
profile: its relationship to `spec.image` is not verified by the agent.

The image path supplies filesystem contents. The explicit workload command
is the complete argv; image `Entrypoint`, `Cmd`, `Env`, `WorkingDir`, `User`,
`Volumes` and other execution defaults are not applied. The local profile
continues to use cwd `/`, its fixed minimal environment, UID/GID 0 mapped to
the host caller, a read-only rootfs and private runtime mounts. This is not a
general Docker-compatible image runner.

## Preparation, limits and cancellation

The agent unpacks once into `SESSION/rootfs`. Replicas and retries use that
same verified read-only tree. Each attempt still gets its own OCI bundle,
identity and runtime state. A new session verifies and extracts again; there
is no reusable rootfs cache.

`--max-image-bytes` defaults to 1 GiB and accepts 1 MiB through 16 GiB. It is
valid only with `--image-layout`. The bound independently limits:

- Total stored layer bytes across the entire image.
- Total uncompressed tar stream bytes, including headers and padding, across
  all layers.
- Extracted file payload bytes, including files subsequently overwritten.

The existing 65536-entry bound also spans all layers. Descriptor documents
remain separately bounded at 16 MiB, with JSON depth/token limits. Blob files
must be regular files; nonblocking opens prevent a FIFO without a writer
from hanging preparation. SHA-256 digests, descriptor byte sizes and every
layer's uncompressed diff ID are checked. Layer extraction and privilege
stripping follow the [fifth-regiment extraction contract](fifth-regiment.md).

The whole-session monotonic deadline now includes image preparation. The
loader checks cancellation before metadata reads and between streaming
chunks, including gzip input and output. Filesystem calls themselves are
synchronous; the deadline is cooperative, not a hard bound on a stalled
filesystem syscall. `UnpackOptions::cancelled` and the optional `inspect`
callback must not throw.

Verification errors, unsupported layers, extraction failures, bounds and
cancellation prevent launch. Preparation failure cancels desired state,
creates no attempt or reservation and consumes no retry budget. With a
journal, this cancellation is recorded; recording failures fail closed.
Partial trees and session directories are retained for inspection and are
never reused. The runtime recovery barrier is marked only before start,
so a preparation failure does not block a fresh session. After a successful
run the tree is retained with the other session evidence. Administrators
must manage disk retention across sessions.

## Result and recovery contract

`LocalExecutionResult` JSON adds a `rootfs` object:

```json
{
  "path": "/absolute/path/to/session/rootfs",
  "source": "image-layout",
  "verified": true,
  "manifestDigest": "sha256:<manifest digest>",
  "configDigest": "sha256:<configuration digest>",
  "layers": 2,
  "files": 10,
  "bytes": 4096
}
```

`verified` becomes true only after complete successful preparation. For a
provisioned rootfs it is false, with empty digests. Failed preparation may
report completed layer counts; empty digests and `verified: false` mean the
tree is unusable. Verification reports describe preparation, not protection
against subsequent changes by the trusted host user. Layouts and session
parents are administrator-managed; same-UID hostile mutation is outside
this local profile's isolation boundary.

The journal retains the workload's manifest pin and normal lifecycle facts.
It does not persist a reusable verification attestation. Recovery never
starts work or trusts an old extracted tree. Existing manual recovery rules
for uncertain execution still apply.

## OCI layout correction and compatibility

The fifth implementation mistakenly read blobs from `blobs/<hex>`. The
loader now uses the OCI layout path **`blobs/sha256/<hex>`** for manifests,
configurations and layers. Flat layouts are rejected, with no fallback.
For layouts created for that earlier implementation, move the SHA-256 blob
files into `blobs/sha256/`; their contents and digests do not change. Standard
OCI layouts already use this path.

Descriptor sizes are now enforced rather than advisory. Regenerate incorrect
descriptors and their containing content-addressed manifests/index before
use; editing a hashed JSON blob also changes its digest. Index
`schemaVersion` must be 2, duplicate JSON keys are rejected, and an explicitly
malformed platform is not treated as an omitted platform. These corrections
also apply to `image inspect/unpack`. `inspect` verifies manifest and config
blobs and validates layer descriptors; `unpack` additionally reads and
verifies the actual layers.

The workload API remains `omnimesh.io/v1alpha1`; journal framing is unchanged.
CLI output fields and C++ options/results are additive; rebuild C++ consumers
against the updated static library. Image building, registries, signatures,
cross-node transfer and workload artifact inputs remain unimplemented.

## Validation

The `images` suite covers standard blob paths, pin selection among images
of the same platform, descriptor sizes, duplicate keys, malformed platforms,
FIFO rejection, aggregate layer/padding limits and mid-stream cancellation.
The `execution` suite covers verified replica and retry bundles, both pin
forms, conflicting sources, platform rejection, deadlines, cancellation,
corruption before launch, and zero-reservation journal recovery after failed
preparation. `agent-cli` builds a tar-backed layout and checks that the
protocol fixture reads the verified bytes through the generated bundle root.
The fixture proves protocol wiring, not real OCI isolation or enforcement.

Validation on October 5, 2026: GCC Debug and Clang AddressSanitizer plus
UndefinedBehaviorSanitizer each passed all 12 runnable CTest suites. Both
skipped the optional `oci-runtime` suite because no real OCI runtime was
configured; neither `crun` nor `runc` was installed in the validation
environment. Targeted clang-tidy analyzer checks on the image loader reported
no diagnostics. `git diff --check` passed. Builds used fresh directories under
`/tmp/opencode` because the existing build cache referenced an unwritable
earlier checkout.

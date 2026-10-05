# Fifth implementation regiment

The fifth regiment adds local OCI image loading and verification to the
supporting services. `ImageLoader` validates an OCI image layout directory,
selects a platform manifest, verifies every blob by digest while streaming,
checks the configuration and layer chain, and unpacks layers into a fresh
rootfs staging directory with a root-confined tar extractor. No registry
access, image building, or worker input wiring has been established.

## Image interface

```sh
omnimesh image inspect --layout /absolute/path/to/layout \
  --platform linux/amd64 [--digest sha256:<64 hex>]
omnimesh image unpack --layout /absolute/path/to/layout \
  --rootfs /absolute/path/to/fresh-rootfs \
  [--platform linux/amd64] [--digest sha256:<64 hex>]
```

The layout is an untrusted administrator-supplied directory following the OCI
image layout: `oci-layout` (version `1.0.0`), `index.json` (an OCI image
index), and content-addressed blobs. `inspect` reports the selected manifest
digest, configuration digest, platform and per-layer digests, media types and
diff IDs without extracting. `unpack` applies each layer in order and reports
the manifest digest, layer count, entry count and unpacked bytes.

Manifest selection matches `os`/`architecture` exactly, defaulting to the
host platform. A single-manifest index may omit the platform. `--digest`
pins the expected manifest digest; anything else is rejected before any blob
is read.

Accepted media types are exactly the OCI v1 set: image index, image manifest,
image configuration, `layer.v1.tar` and `layer.v1.tar+gzip`. Docker media
types and unknown layer codecs are rejected as not implemented rather than
guessed. The configuration's `os`/`architecture` must match the selection,
`rootfs.type` must be `layers`, and the diff-ID count must equal the layer
count. Each blob is hashed while streaming and must match its descriptor
digest; each layer's uncompressed bytes must match its diff ID (for
uncompressed layers the diff ID equals the blob digest). Descriptor `size`
fields are advisory only: the digests decide.

## Extraction contract

`prepare` semantics per layer: gzip layers decompress through an in-tree
DEFLATE decoder (no compression library is vendored) with the unpack bound
enforced incrementally and CRC/length trailers verified; tar layers stream
directly. Every layer must end with its own end-of-archive marker.

The tar extractor (`TarExtractor`) applies regular files, directories,
symlinks, lexically contained hardlinks, GNU long names and OCI whiteouts
(`.wh.*` removal, `.wh..wh..opq` opaque clearing). It rejects absolute
paths, `..` components, symlink traversal while writing, devices, fifos,
sockets, sparse files, checksum damage, non-zero padding and trailing data.
Setuid/setgid bits are stripped and file ownership and timestamps from the
archive are not applied: the tree is staged by the invoking user for rootless
use, and the root itself must not be a symlink. Later entries replace earlier
ones. Directories merge. Unpacked size (default bound 1 GiB) and entry count
(default bound 65536) are enforced before writing.

A failed unpack leaves the partial tree for inspection, exactly like a
failed session directory, and the rootfs path is never reused: unpacking
into an existing directory is a conflict. Corrupt blobs, mismatched digests
and foreign files fail closed and preserve the layout.

## Explicitly out of scope

Registry interaction, tag resolution, signature verification, Docker image
formats, zstd-compressed layers, image building, import/export beyond local
layouts, and staging spool blobs into worker bundles all remain later work.
The node agent still takes an administrator-provisioned `--rootfs`; what this
regiment changes is that the administrator can now produce that rootfs with a
digest-verified `image unpack` instead of verifying it by hand.

## Validation

`images` covers gzip vectors (empty, fixed and dynamic Huffman blocks, file
names, concatenated members, incremental bounds) against system-computed
digests with 1-byte source strides and 7-byte output reads; tar safety
(traversal, absolute paths, symlink escape and traversal refusal, hardlink
containment, whiteouts and opaque clearing, devices, checksum damage,
truncation, trailing data, setuid stripping, quota and count bounds, long
names and ustar prefixes); and layout flows (layered overwrite, whiteouts
across layers, gzip layers with frozen vectors and known identities on both
sides, tampered blobs, wrong diff IDs, platform mismatches, digest pins,
Docker types, missing layouts and rootfs reuse).

`cli` builds a real layout with the system `tar` via CMake, then inspects
with a digest pin, unpacks with byte-identical content checks, and exercises
rootfs reuse, usage errors and missing layouts.

Validation on October 5, 2026: the GCC Debug suite passed twelve tests and
skipped the optional real OCI test. The Clang AddressSanitizer and
UndefinedBehaviorSanitizer suite produced the same result. Targeted
clang-tidy analyzer checks reported no new diagnostics in the loader, tar
and decompression implementations. No real OCI runtime was available for
validation.

## Compatibility

The workload and result API remain `omnimesh.io/v1alpha1`. `image inspect`
and `image unpack` are new commands with JSON output and the standard exit
codes. The on-disk layout consumed is the OCI image layout `1.0.0`; no
migration support is needed or offered. Unpack roots default to mode `0755`;
spool, journal and session conventions are unchanged.

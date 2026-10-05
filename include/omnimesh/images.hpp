#pragma once

#include "omnimesh/artifacts.hpp"
#include "omnimesh/manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace omnimesh {

// OCI media types accepted by the loader. Docker types and unknown layer
// codecs are rejected explicitly rather than guessed.
inline constexpr const char *kOciIndexMediaType =
    "application/vnd.oci.image.index.v1+json";
inline constexpr const char *kOciManifestMediaType =
    "application/vnd.oci.image.manifest.v1+json";
inline constexpr const char *kOciConfigMediaType =
    "application/vnd.oci.image.config.v1+json";
inline constexpr const char *kOciLayerTarMediaType =
    "application/vnd.oci.image.layer.v1.tar";
inline constexpr const char *kOciLayerGzipMediaType =
    "application/vnd.oci.image.layer.v1.tar+gzip";

inline constexpr std::uint64_t kMaxImageBytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxImageFiles = 65536;
inline constexpr std::uint64_t kMaxImageDescriptorBytes = 16ULL * 1024ULL * 1024ULL;

struct ImagePlatform {
  std::string os;           // e.g. "linux"
  std::string architecture; // e.g. "amd64", "arm64"
};

// Host platform from uname: linux/amd64, linux/arm64, or empty on mismatch.
ImagePlatform host_platform();
bool operator==(const ImagePlatform &left, const ImagePlatform &right);

struct TarBounds {
  std::uint64_t max_bytes{kMaxImageBytes};
  std::size_t max_files{kMaxImageFiles};
};

struct TarReport {
  std::uint64_t entries{0};
  std::uint64_t files{0};
  std::uint64_t directories{0};
  std::uint64_t symlinks{0};
  std::uint64_t bytes{0};
};

// Streaming, root-confined tar extraction for OCI layer application.
// Regular files, directories, symlinks, hardlinks (lexically contained),
// GNU long names and OCI whiteouts (`.wh.*`, `.wh..wh..opq`) are applied;
// absolute paths, `..` components, symlink traversal, devices, fifos,
// sockets, sparse files and setuid/setgid bits are rejected or stripped.
// Ownership and timestamps from the archive are not applied: the tree is
// staged by the invoking user for rootless use. Later entries replace
// earlier ones; whiteouts remove. The root directory must already exist and
// must not itself be a symlink.
class TarExtractor {
public:
  TarExtractor() = default;
  TarExtractor(const TarExtractor &) = delete;
  TarExtractor &operator=(const TarExtractor &) = delete;
  ~TarExtractor();

  Status open(const std::string &root, const TarBounds &bounds);
  Status write(const unsigned char *data, std::size_t size);
  Status write(const std::string &data);
  // Requires the two zero-block end marker; trailing data fails. A
  // finished archive re-arms the extractor for the next layer archive.
  Status finish();
  Status close();
  TarReport report() const;

private:
  Status consume();
  Status parse_header(const unsigned char *block);
  Status finish_entry();
  Status apply_metadata();
  Status ensure_parent(const std::string &path);
  Status open_clean(const std::string &path);
  Status remove_tree(const std::string &path);

  int root_fd_{-1};
  TarBounds bounds_;
  TarReport report_{};
  std::string pending_;
  std::string long_name_;
  std::string long_link_;
  bool have_long_name_{false};
  bool have_long_link_{false};
  std::string entry_name_;
  char entry_type_{0};
  std::string entry_link_;
  std::uint64_t entry_mode_{0};
  std::uint64_t entry_left_{0};
  std::uint64_t entry_skip_{0};
  std::string entry_collect_;
  bool entry_zero_pad_{false};
  int entry_fd_{-1};
  bool in_data_{false};
  bool saw_end_{false};
  bool done_{false};
  bool failed_{false};
};

struct ImageLayer {
  std::string digest;
  std::string media_type;
  std::uint64_t size{0};
  std::string diff_id;
};

struct ImageSummary {
  std::string manifest_digest;
  std::string config_digest;
  ImagePlatform platform;
  std::uint64_t config_size{0};
  std::vector<ImageLayer> layers;
};

struct UnpackOptions {
  std::string layout_directory;
  std::string rootfs_directory; // Created fresh; never reused.
  ImagePlatform platform;       // Empty selects the host platform.
  std::string expected_digest;  // Empty skips manifest pinning.
  std::uint64_t max_bytes{kMaxImageBytes};
};

struct UnpackReport {
  std::string manifest_digest;
  std::uint64_t layers{0};
  std::uint64_t files{0};
  std::uint64_t bytes{0};
};

// Local OCI image-layout loader (no registry access). Every blob is
// verified by digest while streaming; descriptors use exact media-type
// matches; the selected platform must match the image config; and each
// layer's uncompressed identity must match the config's diff_id chain.
// `inspect` reports the selected manifest without extracting. `unpack`
// applies layers in order into a fresh directory. A failed unpack leaves
// the partial tree for inspection and never reuses it.
class ImageLoader {
public:
  Status inspect(const std::string &layout, const ImagePlatform &select,
                 const std::string &expected_digest, ImageSummary &summary);
  Status unpack(const UnpackOptions &options, UnpackReport &report);
};

} // namespace omnimesh

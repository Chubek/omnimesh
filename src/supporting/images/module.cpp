#include "omnimesh/images.hpp"
#include "omnimesh/gzip.hpp"
#include "omnimesh/sha256.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace omnimesh {
namespace {

using Json = nlohmann::json;

Status io_error(const char *message) {
  return {StatusCode::internal,
          std::string(message) + ": " + std::strerror(errno)};
}

Status bad_layout(const std::string &what) {
  return {StatusCode::invalid_argument, "invalid image layout: " + what};
}

// Reads an entire small file with a bound. Larger blobs stream elsewhere.
Status read_bounded(const std::string &path, std::uint64_t bound,
                    std::string &data) {
  if (path.empty() || path.size() > 4096 ||
      path.find('\0') != std::string::npos) {
    return {StatusCode::invalid_argument, "invalid layout path"};
  }
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return io_error("cannot open layout file");
  }
  struct stat info{};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    return {StatusCode::invalid_argument, "layout path is not a file"};
  }
  data.clear();
  char chunk[65536];
  std::uint64_t total = 0;
  while (true) {
    const ssize_t count = read(fd, chunk, sizeof(chunk));
    if (count < 0) {
      ::close(fd);
      return io_error("cannot read layout file");
    }
    if (count == 0) {
      break;
    }
    total += static_cast<std::uint64_t>(count);
    if (total > bound) {
      ::close(fd);
      return {StatusCode::resource_exhausted,
              "layout file exceeds its bound"};
    }
    data.append(chunk, static_cast<std::size_t>(count));
  }
  ::close(fd);
  return Status::Ok();
}

Status parse_json(const std::string &text, const std::string &what,
                  Json &value) {
  if (text.empty() || text.size() > kMaxImageDescriptorBytes) {
    return bad_layout(what + " is empty or too large");
  }
  std::size_t tokens = 0;
  try {
    const auto callback = [&](int depth, Json::parse_event_t event,
                              Json &parsed) {
      if (depth > 32 || ++tokens > 16384) {
        throw std::runtime_error("layout JSON exceeds its bounds");
      }
      if (event == Json::parse_event_t::key &&
          parsed.get<std::string>().size() > 256) {
        throw std::runtime_error("layout JSON key is too long");
      }
      return true;
    };
    value = Json::parse(text, callback);
  } catch (const std::exception &) {
    return bad_layout(what + " is not valid JSON");
  }
  if (!value.is_object()) {
    return bad_layout(what + " must be an object");
  }
  return Status::Ok();
}

struct Descriptor {
  std::string media_type;
  std::string digest;
  std::uint64_t size{0};
};

Status parse_descriptor(const Json &value, const std::string &what,
                        Descriptor &out) {
  if (!value.is_object() || !value.contains("mediaType") ||
      !value["mediaType"].is_string() || !value.contains("digest") ||
      !value["digest"].is_string() || !value.contains("size") ||
      !value["size"].is_number_unsigned()) {
    return bad_layout(what + " descriptor is malformed");
  }
  out.media_type = value["mediaType"].get<std::string>();
  out.digest = value["digest"].get<std::string>();
  out.size = value["size"].get<std::uint64_t>();
  if (out.media_type.empty() || out.media_type.size() > 256 ||
      !valid_artifact_digest(out.digest)) {
    return bad_layout(what + " descriptor has invalid fields");
  }
  return Status::Ok();
}

// Streams a blob while hashing; the digest decides, not the byte count.
Status read_blob(const std::string &layout, const Descriptor &wanted,
                 std::string &data, std::uint64_t bound) {
  if (wanted.digest.size() != 71 ||
      wanted.digest.compare(0, 7, "sha256:") != 0) {
    return bad_layout("blob digest is malformed");
  }
  const auto path = layout + "/blobs/" + wanted.digest.substr(7);
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return {StatusCode::not_found,
            "image blob is missing: " + wanted.digest};
  }
  Sha256 hash;
  data.clear();
  char chunk[65536];
  std::uint64_t total = 0;
  Status status = Status::Ok();
  while (true) {
    const ssize_t count = read(fd, chunk, sizeof(chunk));
    if (count < 0) {
      status = io_error("cannot read image blob");
      break;
    }
    if (count == 0) {
      break;
    }
    total += static_cast<std::uint64_t>(count);
    if (total > bound) {
      status = {StatusCode::resource_exhausted,
                "image blob exceeds its bound"};
      break;
    }
    hash.update(chunk, static_cast<std::size_t>(count));
    data.append(chunk, static_cast<std::size_t>(count));
  }
  ::close(fd);
  if (!status.ok()) {
    data.clear();
    return status;
  }
  if ("sha256:" + Sha256::hex(hash.finish()) != wanted.digest) {
    data.clear();
    return {StatusCode::invalid_argument,
            "image blob failed digest verification: " + wanted.digest};
  }
  return Status::Ok();
}

} // namespace

ImagePlatform host_platform() {
  utsname info{};
  if (uname(&info) != 0 || std::string(info.sysname) != "Linux") {
    return {};
  }
  const std::string machine = info.machine;
  ImagePlatform platform;
  platform.os = "linux";
  if (machine == "x86_64") {
    platform.architecture = "amd64";
  } else if (machine == "aarch64") {
    platform.architecture = "arm64";
  }
  return platform;
}

bool operator==(const ImagePlatform &left, const ImagePlatform &right) {
  return left.os == right.os && left.architecture == right.architecture;
}

Status ImageLoader::inspect(const std::string &layout,
                            const ImagePlatform &select,
                            const std::string &expected_digest,
                            ImageSummary &summary) {
  summary = {};
  if (layout.empty() || layout.size() > 4096 ||
      layout.find('\0') != std::string::npos) {
    return {StatusCode::invalid_argument, "invalid layout directory"};
  }
  if (!expected_digest.empty() && !valid_artifact_digest(expected_digest)) {
    return {StatusCode::invalid_argument, "invalid expected digest"};
  }
  ImagePlatform want = select;
  if (want.os.empty() && want.architecture.empty()) {
    want = host_platform();
    if (want.os.empty()) {
      return {StatusCode::unavailable,
              "host platform is unsupported; select one explicitly"};
    }
  } else if (want.os.empty() || want.architecture.empty() ||
             want.os.size() > 64 || want.architecture.size() > 64) {
    return {StatusCode::invalid_argument, "invalid platform selection"};
  }
  std::string text;
  Status status = read_bounded(layout + "/oci-layout", 4096, text);
  if (!status.ok()) {
    return status;
  }
  Json oci_layout;
  status = parse_json(text, "oci-layout", oci_layout);
  if (!status.ok()) {
    return status;
  }
  if (!oci_layout.contains("imageLayoutVersion") ||
      !oci_layout["imageLayoutVersion"].is_string() ||
      oci_layout["imageLayoutVersion"].get<std::string>() != "1.0.0") {
    return bad_layout("unsupported oci-layout version");
  }
  status = read_bounded(layout + "/index.json", kMaxImageDescriptorBytes,
                        text);
  if (!status.ok()) {
    return status;
  }
  Json index;
  status = parse_json(text, "index.json", index);
  if (!status.ok()) {
    return status;
  }
  if (!index.contains("mediaType") || !index["mediaType"].is_string() ||
      index["mediaType"].get<std::string>() != kOciIndexMediaType ||
      !index.contains("manifests") || !index["manifests"].is_array() ||
      index["manifests"].empty() || index["manifests"].size() > 256) {
    return bad_layout("index.json must list OCI manifests");
  }
  Descriptor manifest;
  bool found = false;
  std::string available;
  for (const auto &entry : index["manifests"]) {
    Descriptor candidate;
    if (parse_descriptor(entry, "index manifest", candidate).ok() &&
        candidate.media_type == kOciManifestMediaType) {
      std::string entry_platform = "?/?";
      bool matches = false;
      if (entry.contains("platform") && entry["platform"].is_object() &&
          entry["platform"].contains("os") &&
          entry["platform"]["os"].is_string() &&
          entry["platform"].contains("architecture") &&
          entry["platform"]["architecture"].is_string()) {
        const auto os = entry["platform"]["os"].get<std::string>();
        const auto arch =
            entry["platform"]["architecture"].get<std::string>();
        entry_platform = os + "/" + arch;
        matches = os == want.os && arch == want.architecture;
      } else if (index["manifests"].size() == 1) {
        matches = true; // Single-manifest layouts may omit the platform.
      }
      if (!available.empty()) {
        available += ", ";
      }
      available += entry_platform;
      if (matches && !found) {
        manifest = candidate;
        found = true;
      }
    }
  }
  if (!found) {
    return {StatusCode::not_found,
            "no manifest for " + want.os + "/" + want.architecture +
                " (available: " + available + ")"};
  }
  if (!expected_digest.empty() && manifest.digest != expected_digest) {
    return {StatusCode::invalid_argument,
            "selected manifest does not match the pinned digest"};
  }
  std::string manifest_text;
  status = read_blob(layout, manifest, manifest_text,
                     kMaxImageDescriptorBytes);
  if (!status.ok()) {
    return status;
  }
  Json manifest_json;
  status = parse_json(manifest_text, "manifest", manifest_json);
  if (!status.ok()) {
    return status;
  }
  if (!manifest_json.contains("mediaType") ||
      !manifest_json["mediaType"].is_string() ||
      manifest_json["mediaType"].get<std::string>() !=
          kOciManifestMediaType ||
      !manifest_json.contains("schemaVersion") ||
      !manifest_json["schemaVersion"].is_number_unsigned() ||
      manifest_json["schemaVersion"].get<std::uint64_t>() != 2 ||
      !manifest_json.contains("config") ||
      !manifest_json.contains("layers") ||
      !manifest_json["layers"].is_array() ||
      manifest_json["layers"].size() > 256) {
    return bad_layout("manifest envelope is malformed");
  }
  Descriptor config;
  status = parse_descriptor(manifest_json["config"], "manifest config",
                            config);
  if (!status.ok()) {
    return status;
  }
  if (config.media_type != kOciConfigMediaType) {
    return bad_layout("manifest config must be an OCI image configuration");
  }
  std::string config_text;
  status = read_blob(layout, config, config_text, kMaxImageDescriptorBytes);
  if (!status.ok()) {
    return status;
  }
  Json config_json;
  status = parse_json(config_text, "image configuration", config_json);
  if (!status.ok()) {
    return status;
  }
  if (!config_json.contains("architecture") ||
      !config_json["architecture"].is_string() ||
      !config_json.contains("os") || !config_json["os"].is_string() ||
      !config_json.contains("rootfs") || !config_json["rootfs"].is_object() ||
      !config_json["rootfs"].contains("type") ||
      !config_json["rootfs"]["type"].is_string() ||
      config_json["rootfs"]["type"].get<std::string>() != "layers" ||
      !config_json["rootfs"].contains("diff_ids") ||
      !config_json["rootfs"]["diff_ids"].is_array()) {
    return bad_layout("image configuration is malformed");
  }
  const auto arch = config_json["architecture"].get<std::string>();
  const auto os = config_json["os"].get<std::string>();
  if (os != want.os || arch != want.architecture) {
    return {StatusCode::invalid_argument,
            "image configuration targets " + os + "/" + arch +
                " but " + want.os + "/" + want.architecture +
                " was selected"};
  }
  const auto &diff_ids = config_json["rootfs"]["diff_ids"];
  if (diff_ids.size() != manifest_json["layers"].size()) {
    return bad_layout("layer count does not match diff_id count");
  }
  summary.manifest_digest = manifest.digest;
  summary.config_digest = config.digest;
  summary.platform = want;
  summary.config_size = config.size;
  for (std::size_t i = 0; i < manifest_json["layers"].size(); ++i) {
    Descriptor layer;
    status = parse_descriptor(manifest_json["layers"][i], "layer", layer);
    if (!status.ok()) {
      return status;
    }
    if (layer.media_type != kOciLayerTarMediaType &&
        layer.media_type != kOciLayerGzipMediaType) {
      return {StatusCode::not_implemented,
              "unsupported layer media type: " + layer.media_type};
    }
    if (!diff_ids[i].is_string() ||
        !valid_artifact_digest(diff_ids[i].get<std::string>())) {
      return bad_layout("layer diff_id is malformed");
    }
    ImageLayer entry;
    entry.digest = layer.digest;
    entry.media_type = layer.media_type;
    entry.size = layer.size;
    entry.diff_id = diff_ids[i].get<std::string>();
    summary.layers.push_back(std::move(entry));
  }
  return Status::Ok();
}

Status ImageLoader::unpack(const UnpackOptions &options,
                           UnpackReport &report) {
  report = {};
  if (options.layout_directory.empty() ||
      options.layout_directory.size() > 4096 ||
      options.rootfs_directory.empty() ||
      options.rootfs_directory.size() > 4096 ||
      options.layout_directory.find('\0') != std::string::npos ||
      options.rootfs_directory.find('\0') != std::string::npos) {
    return {StatusCode::invalid_argument, "invalid layout or rootfs path"};
  }
  if (options.max_bytes < 1024ULL * 1024ULL ||
      options.max_bytes > 16ULL * 1024ULL * 1024ULL * 1024ULL) {
    return {StatusCode::invalid_argument, "unpack bound is out of range"};
  }
  ImageSummary summary;
  Status status = inspect(options.layout_directory, options.platform,
                          options.expected_digest, summary);
  if (!status.ok()) {
    return status;
  }
  if (::mkdir(options.rootfs_directory.c_str(), 0755) != 0) {
    if (errno == EEXIST) {
      return {StatusCode::conflict,
              "rootfs directory must be fresh; never reuse a previous unpack"};
    }
    return io_error("cannot create rootfs directory");
  }
  TarExtractor tar;
  TarBounds bounds;
  bounds.max_bytes = options.max_bytes;
  status = tar.open(options.rootfs_directory, bounds);
  if (!status.ok()) {
    return status;
  }
  for (const auto &layer : summary.layers) {
    const auto path = options.layout_directory + "/blobs/" +
                      layer.digest.substr(7);
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
      tar.close();
      return {StatusCode::not_found, "image layer is missing: " + layer.digest};
    }
    Sha256 compressed_hash, plain_hash;
    bool gzipped = layer.media_type == kOciLayerGzipMediaType;
    // Pulls compressed bytes from the blob, verifies the digest, feeds the
    // (possibly decompressed) stream to tar, and verifies the diff_id.
    GzipReader gunzip(
        [&](unsigned char *buffer, std::size_t capacity) -> ssize_t {
          const ssize_t count =
              read(fd, buffer, capacity < 65536 ? capacity : 65536);
          if (count < 0) {
            return -errno;
          }
          if (count > 0) {
            compressed_hash.update(buffer,
                                   static_cast<std::size_t>(count));
          }
          return count;
        },
        options.max_bytes);
    Status layer_status = Status::Ok();
    unsigned char chunk[65536];
    while (true) {
      std::size_t got = 0;
      if (gzipped) {
        layer_status = gunzip.read(chunk, sizeof(chunk), got);
      } else {
        const ssize_t count = read(fd, chunk, sizeof(chunk));
        if (count < 0) {
          layer_status = io_error("cannot read image layer");
        } else {
          got = static_cast<std::size_t>(count);
          if (got > 0) {
            compressed_hash.update(chunk, got);
            plain_hash.update(chunk, got);
          }
        }
      }
      if (!layer_status.ok()) {
        break;
      }
      if (got == 0) {
        break;
      }
      if (gzipped) {
        plain_hash.update(chunk, got);
      }
      layer_status = tar.write(chunk, got);
      if (!layer_status.ok()) {
        break;
      }
    }
    ::close(fd);
    if (layer_status.ok()) {
      if ("sha256:" + Sha256::hex(compressed_hash.finish()) != layer.digest) {
        layer_status = {StatusCode::invalid_argument,
                        "layer failed digest verification: " + layer.digest};
      } else if ("sha256:" + Sha256::hex(plain_hash.finish()) !=
                 layer.diff_id) {
        layer_status = {StatusCode::invalid_argument,
                        "layer does not match its diff_id; layout preserved"};
      } else {
        layer_status = tar.finish();
      }
    }
    if (!layer_status.ok()) {
      tar.close();
      return layer_status;
    }
    ++report.layers;
  }
  status = tar.close();
  if (!status.ok()) {
    return status;
  }
  const auto tar_report = tar.report();
  report.manifest_digest = summary.manifest_digest;
  report.files = tar_report.files + tar_report.directories + tar_report.symlinks;
  report.bytes = tar_report.bytes;
  return Status::Ok();
}

} // namespace omnimesh

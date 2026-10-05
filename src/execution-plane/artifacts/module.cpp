#include "omnimesh/artifacts.hpp"
#include "omnimesh/sha256.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace omnimesh {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kChunkBytes = std::size_t{64} * 1024;

bool valid_tenant(const std::string &tenant) noexcept {
  return !tenant.empty() && tenant.size() <= 256 &&
         tenant.find('\0') == std::string::npos;
}

Status io_error(const char *message) {
  return {StatusCode::internal,
          std::string(message) + ": " + std::strerror(errno)};
}

int sync_parent(const std::string &path) {
  const auto slash = path.rfind('/');
  const auto parent = slash == std::string::npos ? "."
                      : slash == 0               ? "/"
                                                : path.substr(0, slash);
  const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  const int result = fsync(fd);
  ::close(fd);
  return result;
}

Status sync_file(const std::string &path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return io_error("cannot open file for sync");
  }
  const int result = fsync(fd);
  ::close(fd);
  return result == 0 ? Status::Ok() : io_error("cannot sync file");
}

bool is_hex64(const std::string &text) noexcept {
  if (text.size() != 64) {
    return false;
  }
  for (const char c : text) {
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    if (!digit && !lower) {
      return false;
    }
  }
  return true;
}

} // namespace

bool valid_artifact_digest(const std::string &digest) noexcept {
  return digest.size() == 71 && digest.compare(0, 7, "sha256:") == 0 &&
         is_hex64(digest.substr(7));
}

std::string ArtifactSpool::blob_path(const std::string &hex) const {
  return options_.directory + "/blobs/" + hex;
}

Status ArtifactSpool::open(const SpoolOptions &options) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (open_) {
    return {StatusCode::conflict, "spool is already open"};
  }
  // Validate options with directory and capacity bounds before locking.
  if (options.directory.empty() || options.directory.size() > 4096 ||
      options.directory.find('\0') != std::string::npos) {
    return {StatusCode::invalid_argument, "invalid spool directory"};
  }
  if (options.capacity_bytes == 0 ||
      options.capacity_bytes > kDefaultSpoolCapacityBytes * 1024 ||
      options.max_artifacts == 0 || options.max_artifacts > 65536) {
    return {StatusCode::invalid_argument,
            "spool quotas are out of bounds"};
  }
  if (::mkdir(options.directory.c_str(), 0700) != 0 && errno != EEXIST) {
    return io_error("cannot create spool directory");
  }
  struct stat info{};
  if (lstat(options.directory.c_str(), &info) != 0 ||
      !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
      (info.st_mode & 0777) != 0700) {
    return {StatusCode::permission_denied,
            "spool directory must be private and owned by this user"};
  }
  const int fd = ::open(options.directory.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return io_error("cannot open spool directory");
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return {StatusCode::conflict, "spool directory already has an owner"};
  }
  for (const char *child : {"blobs", "tmp"}) {
    const auto path = options.directory + "/" + child;
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
      ::close(fd);
      return io_error("cannot create spool subdirectory");
    }
    if (lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != getuid() || (info.st_mode & 0777) != 0700) {
      ::close(fd);
      return {StatusCode::permission_denied,
              "spool subdirectory must be private and owned by this user"};
    }
  }
  // Drop interrupted staging files; they were never published.
  DIR *staging = opendir((options.directory + "/tmp").c_str());
  if (!staging) {
    ::close(fd);
    return io_error("cannot inspect spool staging area");
  }
  while (dirent *entry = readdir(staging)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    if (name.find('/') != std::string::npos || name.size() > 128) {
      closedir(staging);
      ::close(fd);
      return {StatusCode::invalid_argument,
              "unexpected file in spool staging area"};
    }
    if (unlink((options.directory + "/tmp/" + name).c_str()) != 0) {
      closedir(staging);
      ::close(fd);
      return io_error("cannot clear spool staging area");
    }
  }
  closedir(staging);
  options_ = options;
  lock_fd_ = fd;
  open_ = true;
  index_.clear();
  diagnostics_.clear();
  Status status = reconcile();
  if (!status.ok()) {
    ::close(lock_fd_);
    lock_fd_ = -1;
    open_ = false;
    return status;
  }
  return Status::Ok();
}

Status ArtifactSpool::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) {
    return Status::Ok();
  }
  Status status = save_index();
  if (lock_fd_ >= 0) {
    ::close(lock_fd_);
    lock_fd_ = -1;
  }
  open_ = false;
  return status;
}

Status ArtifactSpool::save_index() {
  Json blobs = Json::object();
  for (const auto &entry : index_) {
    blobs[entry.first.substr(7)] = {{"size", entry.second.size},
                                    {"tenant", entry.second.tenant}};
  }
  const Json document{{"v", 1}, {"blobs", std::move(blobs)}};
  const auto text = document.dump();
  if (text.size() > kMaxArtifactCount * 512) {
    return {StatusCode::internal, "spool index exceeds its bound"};
  }
  const auto staging = options_.directory + "/tmp/index.json";
  const int fd =
      ::open(staging.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return io_error("cannot stage spool index");
  }
  const auto *bytes = text.data();
  std::size_t remaining = text.size();
  while (remaining > 0) {
    const ssize_t written = write(fd, bytes, remaining);
    if (written < 0) {
      const int error = errno;
      ::close(fd);
      unlink(staging.c_str());
      return {StatusCode::internal,
              std::string("cannot write spool index: ") +
                  std::strerror(error)};
    }
    bytes += static_cast<std::size_t>(written);
    remaining -= static_cast<std::size_t>(written);
  }
  if (fsync(fd) != 0) {
    const int error = errno;
    ::close(fd);
    unlink(staging.c_str());
    return {StatusCode::internal,
            std::string("cannot sync spool index: ") + std::strerror(error)};
  }
  ::close(fd);
  if (chmod(staging.c_str(), 0600) != 0) {
    unlink(staging.c_str());
    return io_error("cannot protect spool index");
  }
  const auto target = options_.directory + "/index.json";
  if (rename(staging.c_str(), target.c_str()) != 0) {
    unlink(staging.c_str());
    return io_error("cannot publish spool index");
  }
  Status status = sync_file(target);
  if (status.ok() && sync_parent(target) != 0) {
    status = io_error("cannot sync spool directory");
  }
  return status;
}

Status ArtifactSpool::reconcile() {
  std::map<std::string, ArtifactInfo> disk;
  DIR *store = opendir((options_.directory + "/blobs").c_str());
  if (!store) {
    return io_error("cannot inspect spool store");
  }
  while (dirent *entry = readdir(store)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    if (!is_hex64(name)) {
      closedir(store);
      return {StatusCode::invalid_argument,
              "unexpected file in spool store: " + name};
    }
    const auto path = options_.directory + "/blobs/" + name;
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != getuid()) {
      closedir(store);
      return {StatusCode::invalid_argument,
              "spool blob is not an owned regular file: " + name};
    }
    if (static_cast<std::uint64_t>(info.st_size) > kMaxArtifactBytes) {
      closedir(store);
      return {StatusCode::invalid_argument,
              "spool blob exceeds the size bound: " + name};
    }
    ArtifactInfo found;
    found.digest = "sha256:" + name;
    found.size = static_cast<std::uint64_t>(info.st_size);
    disk.emplace(found.digest, found);
  }
  closedir(store);
  std::map<std::string, ArtifactInfo> rebuilt;
  const auto index_path = options_.directory + "/index.json";
  struct stat index_info{};
  const bool have_index =
      lstat(index_path.c_str(), &index_info) == 0 && S_ISREG(index_info.st_mode);
  Json saved;
  bool index_usable = false;
  if (have_index) {
    const int fd = ::open(index_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
      std::string text;
      char chunk[kChunkBytes];
      bool read_error = false;
      while (true) {
        const ssize_t count = read(fd, chunk, sizeof(chunk));
        if (count < 0) {
          read_error = true;
          break;
        }
        if (count == 0) {
          break;
        }
        text.append(chunk, static_cast<std::size_t>(count));
        if (text.size() > kMaxArtifactCount * 512) {
          break;
        }
      }
      ::close(fd);
      if (!read_error && text.size() <= kMaxArtifactCount * 512) {
        try {
          saved = Json::parse(text);
          index_usable = saved.is_object() && saved.value("v", 0) == 1 &&
                         saved.contains("blobs") &&
                         saved["blobs"].is_object();
        } catch (const std::exception &) {
          index_usable = false;
        }
      }
    }
  }
  if (index_usable) {
    for (const auto &item : saved["blobs"].items()) {
      const std::string digest = "sha256:" + item.key();
      const auto blob = disk.find(digest);
      if (blob == disk.end()) {
        continue; // Blob lost; drop the stale entry below.
      }
      const auto &value = item.value();
      if (!value.is_object() || !value.contains("size") ||
          !value["size"].is_number_unsigned() || !value.contains("tenant") ||
          !value["tenant"].is_string()) {
        continue;
      }
      ArtifactInfo info;
      info.digest = digest;
      info.size = value["size"].get<std::uint64_t>();
      info.tenant = value["tenant"].get<std::string>();
      if (info.size != blob->second.size || !valid_tenant(info.tenant)) {
        continue; // Size or tenant changed out of band; re-hash below.
      }
      rebuilt.emplace(digest, info);
    }
  } else if (have_index && diagnostics_.size() < 64) {
    diagnostics_.push_back(
        {"$spool", "spool index was unreadable and will be rebuilt"});
  }
  for (const auto &entry : disk) {
    if (rebuilt.count(entry.first) != 0) {
      continue;
    }
    // Re-hash to confirm identity; tenant labels reset to empty and must be
    // re-published before tenant-checked fetches succeed.
    const int fd =
        ::open(blob_path(entry.first.substr(7)).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return io_error("cannot re-hash spool blob");
    }
    Sha256 hash;
    char chunk[kChunkBytes];
    std::uint64_t hashed = 0;
    bool read_error = false;
    while (true) {
      const ssize_t count = read(fd, chunk, sizeof(chunk));
      if (count < 0) {
        read_error = true;
        break;
      }
      if (count == 0) {
        break;
      }
      hash.update(chunk, static_cast<std::size_t>(count));
      hashed += static_cast<std::uint64_t>(count);
    }
    ::close(fd);
    if (read_error) {
      return io_error("cannot re-hash spool blob");
    }
    if (hashed != entry.second.size ||
        "sha256:" + Sha256::hex(hash.finish()) != entry.first) {
      return {StatusCode::invalid_argument,
              "spool blob failed verification during open; spool preserved"};
    }
    ArtifactInfo info = entry.second;
    info.tenant.clear();
    rebuilt.emplace(entry.first, info);
    if (diagnostics_.size() < 64) {
      diagnostics_.push_back(
          {"$spool", "re-hashed unindexed blob " + entry.first});
    }
  }
  index_ = std::move(rebuilt);
  return save_index();
}

Status ArtifactSpool::put_bytes(std::string_view data,
                                const std::string &tenant,
                                ArtifactInfo &info) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) {
    return {StatusCode::unavailable, "spool is not open"};
  }
  if (!valid_tenant(tenant)) {
    return {StatusCode::invalid_argument, "invalid tenant label"};
  }
  if (data.size() > kMaxArtifactBytes) {
    return {StatusCode::invalid_argument,
            "artifact exceeds the 64 MiB size bound"};
  }
  Sha256 hash;
  hash.update(data);
  const std::string hex = Sha256::hex(hash.finish());
  const std::string digest = "sha256:" + hex;
  const auto path = blob_path(hex);
  struct stat existing{};
  if (lstat(path.c_str(), &existing) == 0) {
    if (!S_ISREG(existing.st_mode) ||
        static_cast<std::uint64_t>(existing.st_size) != data.size()) {
      return {StatusCode::invalid_argument,
              "spool blob conflicts with a foreign file; spool preserved"};
    }
    info = {digest, data.size(), tenant};
    index_[digest] = info;
    return save_index();
  }
  std::uint64_t used = 0;
  for (const auto &entry : index_) {
    used += entry.second.size;
  }
  if (index_.size() + 1 > options_.max_artifacts ||
      used + data.size() > options_.capacity_bytes) {
    return {StatusCode::resource_exhausted,
            "spool quota reached; collect unreferenced artifacts first"};
  }
  const auto staging = options_.directory + "/tmp/put-" + hex;
  const int fd =
      ::open(staging.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) {
    return io_error("cannot stage artifact");
  }
  const auto *bytes = data.data();
  std::size_t remaining = data.size();
  Status status = Status::Ok();
  while (remaining > 0) {
    const ssize_t written = write(fd, bytes, remaining);
    if (written < 0) {
      status = io_error("cannot write staged artifact");
      break;
    }
    bytes += static_cast<std::size_t>(written);
    remaining -= static_cast<std::size_t>(written);
  }
  if (status.ok() && fsync(fd) != 0) {
    status = io_error("cannot sync staged artifact");
  }
  ::close(fd);
  if (!status.ok()) {
    unlink(staging.c_str());
    return status;
  }
  if (rename(staging.c_str(), path.c_str()) != 0) {
    unlink(staging.c_str());
    return io_error("cannot publish artifact");
  }
  chmod(path.c_str(), 0400);
  status = sync_file(path);
  if (status.ok() && sync_parent(path) != 0) {
    status = io_error("cannot sync spool store");
  }
  if (!status.ok()) {
    return status;
  }
  info = {digest, data.size(), tenant};
  index_[digest] = info;
  return save_index();
}

Status ArtifactSpool::put(const std::string &path, const std::string &tenant,
                          ArtifactInfo &info) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
      return {StatusCode::unavailable, "spool is not open"};
    }
    if (!valid_tenant(tenant)) {
      return {StatusCode::invalid_argument, "invalid tenant label"};
    }
  }
  if (path.empty() || path.size() > 4096 ||
      path.find('\0') != std::string::npos) {
    return {StatusCode::invalid_argument, "invalid artifact source path"};
  }
  const int source = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (source < 0) {
    return io_error("cannot open artifact source");
  }
  struct stat source_info{};
  if (fstat(source, &source_info) != 0 || !S_ISREG(source_info.st_mode)) {
    ::close(source);
    return {StatusCode::invalid_argument,
            "artifact source must be a regular file"};
  }
  // Hash while streaming into staging so memory stays bounded; the digest is
  // known before anything is published.
  char staging_template[4096 + 32];
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto prefix = options_.directory + "/tmp/stage-";
    if (prefix.size() + 7 >= sizeof(staging_template)) {
      ::close(source);
      return {StatusCode::invalid_argument, "spool directory path too long"};
    }
    std::snprintf(staging_template, sizeof(staging_template), "%sXXXXXX",
                  prefix.c_str());
  }
  const int staged = mkstemp(staging_template);
  if (staged < 0) {
    ::close(source);
    return io_error("cannot stage artifact");
  }
  Sha256 hash;
  char chunk[kChunkBytes];
  std::uint64_t size = 0;
  Status status = Status::Ok();
  while (true) {
    const ssize_t count = read(source, chunk, sizeof(chunk));
    if (count < 0) {
      status = io_error("cannot read artifact source");
      break;
    }
    if (count == 0) {
      break;
    }
    if (size + static_cast<std::uint64_t>(count) > kMaxArtifactBytes) {
      status = {StatusCode::invalid_argument,
                "artifact exceeds the 64 MiB size bound"};
      break;
    }
    hash.update(chunk, static_cast<std::size_t>(count));
    const auto *out = chunk;
    std::size_t remaining = static_cast<std::size_t>(count);
    while (remaining > 0) {
      const ssize_t written = write(staged, out, remaining);
      if (written < 0) {
        status = io_error("cannot write staged artifact");
        break;
      }
      out += static_cast<std::size_t>(written);
      remaining -= static_cast<std::size_t>(written);
    }
    if (!status.ok()) {
      break;
    }
    size += static_cast<std::uint64_t>(count);
  }
  ::close(source);
  if (status.ok() && fsync(staged) != 0) {
    status = io_error("cannot sync staged artifact");
  }
  ::close(staged);
  if (!status.ok()) {
    unlink(staging_template);
    return status;
  }
  const std::string hex = Sha256::hex(hash.finish());
  const std::string digest = "sha256:" + hex;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) {
    unlink(staging_template);
    return {StatusCode::unavailable, "spool was closed during staging"};
  }
  const auto path_final = blob_path(hex);
  struct stat existing{};
  if (lstat(path_final.c_str(), &existing) == 0) {
    unlink(staging_template);
    if (!S_ISREG(existing.st_mode) ||
        static_cast<std::uint64_t>(existing.st_size) != size) {
      return {StatusCode::invalid_argument,
              "spool blob conflicts with a foreign file; spool preserved"};
    }
    info = {digest, size, tenant};
    index_[digest] = info;
    return save_index();
  }
  std::uint64_t used = 0;
  for (const auto &entry : index_) {
    used += entry.second.size;
  }
  if (index_.size() + 1 > options_.max_artifacts ||
      used + size > options_.capacity_bytes) {
    unlink(staging_template);
    return {StatusCode::resource_exhausted,
            "spool quota reached; collect unreferenced artifacts first"};
  }
  if (rename(staging_template, path_final.c_str()) != 0) {
    unlink(staging_template);
    return io_error("cannot publish artifact");
  }
  chmod(path_final.c_str(), 0400);
  status = sync_file(path_final);
  if (status.ok() && sync_parent(path_final) != 0) {
    status = io_error("cannot sync spool store");
  }
  if (!status.ok()) {
    return status;
  }
  info = {digest, size, tenant};
  index_[digest] = info;
  return save_index();
}

Status ArtifactSpool::fetch(const std::string &digest,
                            const std::string &tenant,
                            const std::string &out_path) {
  std::string source;
  std::uint64_t size = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
      return {StatusCode::unavailable, "spool is not open"};
    }
    if (!valid_artifact_digest(digest)) {
      return {StatusCode::invalid_argument, "invalid artifact digest"};
    }
    const auto found = index_.find(digest);
    if (found == index_.end()) {
      return {StatusCode::not_found, "artifact is not in the spool"};
    }
    if (found->second.tenant != tenant) {
      return {StatusCode::permission_denied,
              "artifact belongs to another tenant"};
    }
    source = blob_path(digest.substr(7));
    size = found->second.size;
  }
  if (out_path.empty() || out_path.size() > 4096 ||
      out_path.find('\0') != std::string::npos) {
    return {StatusCode::invalid_argument, "invalid artifact output path"};
  }
  const int blob = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (blob < 0) {
    return {StatusCode::unavailable,
            "stored blob is unreachable; spool may need recovery"};
  }
  const auto staging = out_path + ".omnimesh-part";
  if (staging.size() > 4096 + 16) {
    ::close(blob);
    return {StatusCode::invalid_argument, "artifact output path too long"};
  }
  const int out =
      ::open(staging.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
  if (out < 0) {
    ::close(blob);
    return io_error("cannot stage artifact output");
  }
  Sha256 hash;
  char chunk[kChunkBytes];
  std::uint64_t copied = 0;
  Status status = Status::Ok();
  while (true) {
    const ssize_t count = read(blob, chunk, sizeof(chunk));
    if (count < 0) {
      status = io_error("cannot read stored blob");
      break;
    }
    if (count == 0) {
      break;
    }
    if (copied + static_cast<std::uint64_t>(count) > size) {
      status = {StatusCode::invalid_argument,
                "stored blob is larger than indexed; spool preserved"};
      break;
    }
    hash.update(chunk, static_cast<std::size_t>(count));
    const auto *bytes = chunk;
    std::size_t remaining = static_cast<std::size_t>(count);
    while (remaining > 0) {
      const ssize_t written = write(out, bytes, remaining);
      if (written < 0) {
        status = io_error("cannot write artifact output");
        break;
      }
      bytes += static_cast<std::size_t>(written);
      remaining -= static_cast<std::size_t>(written);
    }
    if (!status.ok()) {
      break;
    }
    copied += static_cast<std::uint64_t>(count);
  }
  ::close(blob);
  if (status.ok() && fsync(out) != 0) {
    status = io_error("cannot sync artifact output");
  }
  ::close(out);
  if (!status.ok()) {
    unlink(staging.c_str());
    return status;
  }
  // Verification happens before the output is published: a mismatch leaves
  // the stored blob untouched and removes only the staged copy.
  if (copied != size || "sha256:" + Sha256::hex(hash.finish()) != digest) {
    unlink(staging.c_str());
    return {StatusCode::invalid_argument,
            "stored blob failed verification; spool preserved"};
  }
  if (rename(staging.c_str(), out_path.c_str()) != 0) {
    unlink(staging.c_str());
    return io_error("cannot publish artifact output");
  }
  Status synced = sync_file(out_path);
  if (synced.ok() && sync_parent(out_path) != 0) {
    synced = io_error("cannot sync artifact output directory");
  }
  return synced;
}

Status ArtifactSpool::fetch_bytes(const std::string &digest,
                                 const std::string &tenant,
                                 std::string &data) {
  std::string source;
  std::uint64_t size = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
      return {StatusCode::unavailable, "spool is not open"};
    }
    if (!valid_artifact_digest(digest)) {
      return {StatusCode::invalid_argument, "invalid artifact digest"};
    }
    const auto found = index_.find(digest);
    if (found == index_.end()) {
      return {StatusCode::not_found, "artifact is not in the spool"};
    }
    if (found->second.tenant != tenant) {
      return {StatusCode::permission_denied,
              "artifact belongs to another tenant"};
    }
    source = blob_path(digest.substr(7));
    size = found->second.size;
  }
  if (size > kMaxArtifactBytes) {
    return {StatusCode::invalid_argument,
            "indexed artifact exceeds the size bound; spool preserved"};
  }
  const int blob = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (blob < 0) {
    return {StatusCode::unavailable,
            "stored blob is unreachable; spool may need recovery"};
  }
  Sha256 hash;
  std::string content;
  content.resize(static_cast<std::size_t>(size));
  std::size_t filled = 0;
  Status status = Status::Ok();
  while (filled < content.size()) {
    const ssize_t count =
        read(blob, content.data() + filled, content.size() - filled);
    if (count < 0) {
      status = io_error("cannot read stored blob");
      break;
    }
    if (count == 0) {
      break;
    }
    hash.update(content.data() + filled, static_cast<std::size_t>(count));
    filled += static_cast<std::size_t>(count);
  }
  // A longer-than-indexed blob fails here; the buffered prefix is discarded.
  char extra = 0;
  const ssize_t tail = read(blob, &extra, 1);
  ::close(blob);
  if (!status.ok()) {
    return status;
  }
  if (tail != 0 || filled != content.size() ||
      "sha256:" + Sha256::hex(hash.finish()) != digest) {
    return {StatusCode::invalid_argument,
            "stored blob failed verification; spool preserved"};
  }
  data = std::move(content);
  return Status::Ok();
}

Status ArtifactSpool::inspect(const std::string &digest,
                              ArtifactInfo &info) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) {
    return {StatusCode::unavailable, "spool is not open"};
  }
  if (!valid_artifact_digest(digest)) {
    return {StatusCode::invalid_argument, "invalid artifact digest"};
  }
  const auto found = index_.find(digest);
  if (found == index_.end()) {
    return {StatusCode::not_found, "artifact is not in the spool"};
  }
  info = found->second;
  return Status::Ok();
}

std::vector<ArtifactInfo> ArtifactSpool::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ArtifactInfo> result;
  if (!open_) {
    return result;
  }
  result.reserve(index_.size());
  for (const auto &entry : index_) {
    result.push_back(entry.second);
  }
  return result; // std::map iteration is already ordered by digest.
}

Status ArtifactSpool::collect(const std::set<std::string> &keep,
                              CollectReport &report) {
  std::lock_guard<std::mutex> lock(mutex_);
  report = {};
  if (!open_) {
    return {StatusCode::unavailable, "spool is not open"};
  }
  for (const auto &digest : keep) {
    if (!valid_artifact_digest(digest)) {
      return {StatusCode::invalid_argument,
              "invalid kept artifact digest: " + digest};
    }
  }
  for (auto it = index_.begin(); it != index_.end();) {
    if (keep.count(it->first) != 0) {
      ++it;
      continue;
    }
    const auto path = blob_path(it->first.substr(7));
    if (chmod(path.c_str(), 0600) != 0 && errno != ENOENT) {
      return io_error("cannot unlock artifact for collection");
    }
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
      return io_error("cannot remove unreferenced artifact");
    }
    ++report.removed;
    report.reclaimed_bytes += it->second.size;
    it = index_.erase(it);
  }
  Status status = save_index();
  if (status.ok() && sync_parent(options_.directory + "/blobs") != 0) {
    status = io_error("cannot sync spool store");
  }
  return status;
}

SpoolStats ArtifactSpool::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  SpoolStats result;
  result.capacity_bytes = options_.capacity_bytes;
  result.max_artifacts = options_.max_artifacts;
  if (!open_) {
    return result;
  }
  result.blobs = index_.size();
  for (const auto &entry : index_) {
    result.bytes += entry.second.size;
  }
  return result;
}

std::vector<Diagnostic> ArtifactSpool::diagnostics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return diagnostics_;
}

} // namespace omnimesh

#include "omnimesh/images.hpp"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace omnimesh {
namespace {

constexpr std::size_t kBlockBytes = std::size_t{512};
constexpr std::size_t kMaxPendingBytes = std::size_t{1024} * 1024;

Status invalid(const std::string &what) {
  return {StatusCode::invalid_argument,
          "unsafe or malformed archive entry: " + what};
}

Status io_error(const char *message) {
  return {StatusCode::internal,
          std::string(message) + ": " + std::strerror(errno)};
}

// Strict octal: leading spaces and NUL padding are allowed, anything else
// fails closed instead of guessing.
bool parse_octal(const unsigned char *field, std::size_t width,
                 std::uint64_t &value) {
  value = 0;
  std::size_t i = 0;
  while (i < width && (field[i] == ' ' || field[i] == '\0')) {
    ++i;
  }
  bool digits = false;
  for (; i < width && field[i] != '\0' && field[i] != ' '; ++i) {
    if (field[i] < '0' || field[i] > '7') {
      return false;
    }
    digits = true;
    value = value * 8 + static_cast<std::uint64_t>(field[i] - '0');
    if (value > (std::uint64_t{1} << 33)) {
      return false;
    }
  }
  return digits;
}

// Splits a tar path into validated components. Absolute paths, empty, `.`
// and `..` components and overlong names never reach the filesystem.
bool split_clean(const std::string &path, std::vector<std::string> &parts) {
  if (path.empty() || path.size() > 4096 || path[0] == '/' ||
      path.find('\0') != std::string::npos) {
    return false;
  }
  std::string rest = path;
  while (rest.size() > 1 && rest.back() == '/') {
    rest.pop_back();
  }
  std::size_t start = 0;
  while (start < rest.size() && rest.compare(start, 2, "./") == 0) {
    start += 2;
  }
  if (start >= rest.size()) {
    return false;
  }
  parts.clear();
  std::size_t at = start;
  while (at <= rest.size()) {
    const auto slash = rest.find('/', at);
    const auto end = slash == std::string::npos ? rest.size() : slash;
    const auto part = rest.substr(at, end - at);
    if (part.empty() || part == "." || part == ".." || part.size() > 255) {
      return false;
    }
    parts.push_back(part);
    if (slash == std::string::npos) {
      break;
    }
    at = slash + 1;
  }
  return !parts.empty();
}

std::string join(const std::vector<std::string> &parts, std::size_t end) {
  std::string path;
  for (std::size_t i = 0; i < end; ++i) {
    if (i > 0) {
      path.push_back('/');
    }
    path += parts[i];
  }
  return path;
}

} // namespace

TarExtractor::~TarExtractor() {
  if (entry_fd_ >= 0) {
    ::close(entry_fd_);
  }
  if (root_fd_ >= 0) {
    ::close(root_fd_);
  }
}

Status TarExtractor::open(const std::string &root, const TarBounds &bounds) {
  if (root_fd_ >= 0) {
    return {StatusCode::conflict, "extractor is already open"};
  }
  if (root.empty() || root.size() > 4096 ||
      root.find('\0') != std::string::npos) {
    return {StatusCode::invalid_argument, "invalid extraction root"};
  }
  if (bounds.max_bytes == 0 || bounds.max_files == 0 ||
      bounds.max_bytes > 16ULL * 1024ULL * 1024ULL * 1024ULL ||
      bounds.max_files > 1048576) {
    return {StatusCode::invalid_argument, "extraction bounds are out of range"};
  }
  const int fd = ::open(root.c_str(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return io_error("cannot open extraction root");
  }
  struct stat info{};
  if (fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode)) {
    ::close(fd);
    return {StatusCode::invalid_argument, "extraction root is not a directory"};
  }
  root_fd_ = fd;
  bounds_ = bounds;
  return Status::Ok();
}

Status TarExtractor::write(const unsigned char *data, std::size_t size) {
  if (root_fd_ < 0) {
    return {StatusCode::unavailable, "extractor is not open"};
  }
  if (failed_) {
    return {StatusCode::unavailable, "extractor has already failed"};
  }
  if (data == nullptr && size != 0) {
    return {StatusCode::invalid_argument, "archive input needs a buffer"};
  }
  if (pending_.size() + size > kMaxPendingBytes) {
    failed_ = true;
    return {StatusCode::resource_exhausted,
            "archive input exceeds its buffering bound"};
  }
  pending_.append(reinterpret_cast<const char *>(data), size);
  Status status = consume();
  if (!status.ok()) {
    failed_ = true;
  }
  return status;
}

Status TarExtractor::write(const std::string &data) {
  return write(reinterpret_cast<const unsigned char *>(data.data()),
               data.size());
}

Status TarExtractor::consume() {
  while (true) {
    if (in_data_) {
      const std::uint64_t total = entry_left_ + entry_skip_;
      const std::size_t want =
          total < pending_.size() ? static_cast<std::size_t>(total)
                                  : pending_.size();
      const std::size_t file_take = entry_fd_ >= 0
                                            ? (entry_left_ < want
                                                   ? static_cast<std::size_t>(
                                                         entry_left_)
                                                   : want)
                                            : 0;
      std::size_t done = 0;
      while (done < file_take) {
        const ssize_t written =
            ::write(entry_fd_, pending_.data() + done, file_take - done);
        if (written < 0) {
          return io_error("cannot write extracted file");
        }
        done += static_cast<std::size_t>(written);
      }
      // Remaining payload (long names, pax records) is collected bounded;
      // padding must be zero.
      const bool collect = entry_fd_ < 0 && !entry_zero_pad_;
      const std::size_t payload_left =
          static_cast<std::size_t>(entry_left_ - file_take);
      const std::size_t rest = want - file_take;
      const std::size_t collect_take =
          collect ? (rest < payload_left ? rest : payload_left) : 0;
      for (std::size_t i = 0; i < collect_take; ++i) {
        entry_collect_.push_back(pending_[done + i]);
        if (entry_collect_.size() > 4096) {
          return invalid("archive metadata entry is too large");
        }
      }
      for (std::size_t i = collect_take; i < rest; ++i) {
        if (pending_[done + i] != 0) {
          return invalid("non-zero archive padding");
        }
      }
      pending_.erase(0, want);
      entry_left_ -= file_take + collect_take;
      entry_skip_ -= (rest - collect_take);
      if (entry_left_ + entry_skip_ > 0) {
        return Status::Ok(); // Need more input.
      }
      in_data_ = false;
      Status status = finish_entry();
      if (!status.ok()) {
        return status;
      }
      continue;
    }
    if (pending_.size() < kBlockBytes) {
      if (done_) {
        // After the end marker only zero padding may follow; anything
        // else fails right away instead of lingering in the buffer.
        for (const char byte : pending_) {
          if (byte != 0) {
            return invalid("trailing data after end of archive");
          }
        }
        pending_.clear();
      }
      return Status::Ok(); // Need more input.
    }
    // Copy the header out: erasing pending_ below invalidates pointers
    // into it.
    unsigned char block[kBlockBytes];
    std::memcpy(block, pending_.data(), kBlockBytes);
    bool zero = true;
    for (std::size_t i = 0; i < kBlockBytes; ++i) {
      if (block[i] != 0) {
        zero = false;
        break;
      }
    }
    pending_.erase(0, kBlockBytes);
    if (zero) {
      if (saw_end_) {
        done_ = true;
      } else {
        saw_end_ = true;
      }
      continue;
    }
    if (done_) {
      return invalid("trailing data after end of archive");
    }
    saw_end_ = false;
    Status status = parse_header(block);
    if (!status.ok()) {
      return status;
    }
  }
}

Status TarExtractor::parse_header(const unsigned char *block) {
  if (report_.entries + 1 > bounds_.max_files) {
    return {StatusCode::resource_exhausted,
            "archive exceeds its entry bound"};
  }
  std::uint64_t mode = 0, size = 0, checksum = 0;
  if (!parse_octal(block + 100, 8, mode) ||
      !parse_octal(block + 124, 12, size) ||
      !parse_octal(block + 148, 8, checksum)) {
    return invalid("bad numeric archive header");
  }
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < kBlockBytes; ++i) {
    sum += (i >= 148 && i < 156) ? ' ' : block[i];
  }
  if (sum != checksum) {
    return invalid("archive header checksum mismatch");
  }
  if (std::memcmp(block + 257, "ustar", 5) != 0) {
    return invalid("archive header is not ustar");
  }
  const char type = static_cast<char>(block[156]);
  std::string name(reinterpret_cast<const char *>(block), 100);
  name.resize(name.find('\0') == std::string::npos ? 100
                                                   : name.find('\0'));
  std::string prefix(reinterpret_cast<const char *>(block + 345), 155);
  prefix.resize(prefix.find('\0') == std::string::npos ? 155
                                                       : prefix.find('\0'));
  if (!prefix.empty()) {
    name = prefix + "/" + name;
  }
  std::string link(reinterpret_cast<const char *>(block + 157), 100);
  link.resize(link.find('\0') == std::string::npos ? 100 : link.find('\0'));
  if (type == 'L' || type == 'K') {
    // GNU long name/link: the payload carries the real value.
    entry_name_.clear();
    entry_type_ = type;
    entry_left_ = size;
    entry_skip_ = (kBlockBytes - (size % kBlockBytes)) % kBlockBytes;
    entry_collect_.clear();
    entry_zero_pad_ = false;
    entry_fd_ = -1;
    if (size == 0 || size > 4096) {
      return invalid("bad long-name archive entry");
    }
    in_data_ = true;
    return Status::Ok();
  }
  if (have_long_name_) {
    name = long_name_;
    have_long_name_ = false;
  }
  if (have_long_link_) {
    link = long_link_;
    have_long_link_ = false;
  }
  if (type == 'x' || type == 'g') {
    // pax extended headers carry metadata the extractor ignores.
    entry_name_.clear();
    entry_type_ = type;
    entry_left_ = size;
    entry_skip_ = (kBlockBytes - (size % kBlockBytes)) % kBlockBytes;
    entry_collect_.clear();
    entry_zero_pad_ = false;
    entry_fd_ = -1;
    in_data_ = true;
    return Status::Ok();
  }
  std::vector<std::string> parts;
  if (!split_clean(name, parts)) {
    return invalid("unsafe archive path: " + name);
  }
  const std::string clean = join(parts, parts.size());
  const auto leaf = parts.back();
  const bool whiteout =
      leaf.size() > 4 && leaf.compare(0, 4, ".wh.") == 0;
  if ((type == '0' || type == '\0') && !whiteout) {
    if (report_.bytes + size > bounds_.max_bytes) {
      return {StatusCode::resource_exhausted,
              "archive exceeds its unpacked-size bound"};
    }
    report_.bytes += size;
    Status status = ensure_parent(clean);
    if (!status.ok()) {
      return status;
    }
    const auto leaf = parts.back();
    const auto parent = join(parts, parts.size() - 1);
    const int parent_fd =
        parent.empty()
            ? ::dup(root_fd_)
            : ::openat(root_fd_, parent.c_str(),
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent_fd < 0) {
      return io_error("cannot open extraction parent");
    }
    if (unlinkat(parent_fd, leaf.c_str(), 0) != 0 && errno != ENOENT) {
      ::close(parent_fd);
      return io_error("cannot replace existing archive path");
    }
    const int fd = ::openat(parent_fd, leaf.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                            0600);
    ::close(parent_fd);
    if (fd < 0) {
      return io_error("cannot create extracted file");
    }
    entry_name_ = clean;
    entry_type_ = '0';
    entry_mode_ = mode & 0777;
    entry_left_ = size;
    entry_skip_ = (kBlockBytes - (size % kBlockBytes)) % kBlockBytes;
    entry_collect_.clear();
    entry_zero_pad_ = true;
    entry_fd_ = fd;
    in_data_ = true;
    return Status::Ok();
  }
  if (size != 0) {
    return invalid("sized non-file archive entry");
  }
  if (type != '5' && type != '2' && type != '1' && !whiteout) {
    return invalid("unsupported archive entry type");
  }
  entry_name_ = clean;
  entry_type_ = type;
  entry_link_ = link;
  entry_mode_ = mode & 0777;
  entry_left_ = 0;
  entry_skip_ = 0;
  entry_collect_.clear();
  entry_zero_pad_ = false;
  entry_fd_ = -1;
  in_data_ = false;
  // Metadata entries apply immediately; they carry no payload.
  return finish_entry();
}

Status TarExtractor::finish_entry() {
  if (entry_type_ == 'L') {
    const auto end = entry_collect_.find('\0');
    long_name_ = entry_collect_.substr(0, end);
    if (long_name_.empty() || long_name_.size() > 4096) {
      return invalid("bad long-name archive entry");
    }
    have_long_name_ = true;
    return Status::Ok();
  }
  if (entry_type_ == 'K') {
    const auto end = entry_collect_.find('\0');
    long_link_ = entry_collect_.substr(0, end);
    if (long_link_.size() > 4096) {
      return invalid("bad long-link archive entry");
    }
    have_long_link_ = true;
    return Status::Ok();
  }
  if (entry_type_ == 'x' || entry_type_ == 'g') {
    return Status::Ok(); // pax metadata is ignored.
  }
  std::vector<std::string> routed;
  bool whiteout = false;
  if (split_clean(entry_name_, routed)) {
    const auto &leaf = routed.back();
    whiteout = leaf.size() > 4 && leaf.compare(0, 4, ".wh.") == 0;
  }
  if (entry_type_ == '0' && !whiteout) {
    if (entry_fd_ >= 0) {
      if (fchmod(entry_fd_, static_cast<mode_t>(entry_mode_)) != 0) {
        ::close(entry_fd_);
        entry_fd_ = -1;
        return io_error("cannot set extracted file mode");
      }
      ::close(entry_fd_);
      entry_fd_ = -1;
    }
    ++report_.entries;
    ++report_.files;
    // Bytes were counted from the header before writing.
    return Status::Ok();
  }
  Status status = apply_metadata();
  if (!status.ok()) {
    return status;
  }
  ++report_.entries;
  return Status::Ok();
}

Status TarExtractor::apply_metadata() {
  std::vector<std::string> parts;
  if (!split_clean(entry_name_, parts)) {
    return invalid("unsafe archive path: " + entry_name_);
  }
  const std::string clean = join(parts, parts.size());
  const auto leaf = parts.back();
  const auto parent = join(parts, parts.size() - 1);
  // Whiteouts remove; opaque markers clear their directory.
  if (leaf.compare(0, 4, ".wh.") == 0) {
    if (leaf == ".wh..wh..opq") {
      const auto target = parent.empty() ? "." : parent;
      const int fd =
          ::openat(root_fd_, target.c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) {
        if (errno == ENOENT) {
          return Status::Ok();
        }
        return io_error("cannot open opaque directory");
      }
      DIR *dir = fdopendir(fd);
      if (!dir) {
        ::close(fd);
        return io_error("cannot inspect opaque directory");
      }
      Status status = Status::Ok();
      while (dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
          continue;
        }
        status = remove_tree(target == "." ? name : target + "/" + name);
        if (!status.ok()) {
          break;
        }
      }
      closedir(dir);
      return status;
    }
    const auto victim = leaf.substr(4);
    if (victim.empty() || victim == "." || victim == ".." ||
        victim.find('/') != std::string::npos) {
      return invalid("bad whiteout archive entry");
    }
    const auto path = parent.empty() ? victim : parent + "/" + victim;
    Status status = remove_tree(path);
    if (!status.ok() && status.code != StatusCode::not_found) {
      return status;
    }
    return Status::Ok();
  }
  if (entry_type_ == '5') {
    Status status = ensure_parent(clean);
    if (!status.ok()) {
      return status;
    }
    const int parent_fd =
        parent.empty()
            ? ::dup(root_fd_)
            : ::openat(root_fd_, parent.c_str(),
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent_fd < 0) {
      return io_error("cannot open extraction parent");
    }
    struct stat known{};
    if (fstatat(parent_fd, leaf.c_str(), &known, AT_SYMLINK_NOFOLLOW) == 0) {
      ::close(parent_fd);
      if (!S_ISDIR(known.st_mode)) {
        return invalid("archive directory collides with a file: " + clean);
      }
      return open_clean(clean);
    }
    if (mkdirat(parent_fd, leaf.c_str(), 0755) != 0) {
      ::close(parent_fd);
      return io_error("cannot create extracted directory");
    }
    ::close(parent_fd);
    Status opened = open_clean(clean);
    if (!opened.ok()) {
      return opened;
    }
    ++report_.directories;
    return Status::Ok();
  }
  if (entry_type_ == '2') {
    if (entry_link_.empty() || entry_link_.size() > 4096 ||
        entry_link_.find('\0') != std::string::npos) {
      return invalid("bad symlink archive entry");
    }
    Status status = ensure_parent(clean);
    if (!status.ok()) {
      return status;
    }
    const int parent_fd =
        parent.empty()
            ? ::dup(root_fd_)
            : ::openat(root_fd_, parent.c_str(),
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent_fd < 0) {
      return io_error("cannot open extraction parent");
    }
    if (unlinkat(parent_fd, leaf.c_str(), 0) != 0 && errno != ENOENT) {
      ::close(parent_fd);
      return io_error("cannot replace existing archive path");
    }
    // Absolute and relative targets are both standard inside images. The
    // extractor never traverses a symlink component while writing, so a
    // link created here cannot redirect a later entry.
    const int created =
        symlinkat(entry_link_.c_str(), parent_fd, leaf.c_str());
    ::close(parent_fd);
    if (created != 0) {
      return io_error("cannot create extracted symlink");
    }
    ++report_.symlinks;
    return Status::Ok();
  }
  if (entry_type_ == '1') {
    std::vector<std::string> target_parts;
    if (entry_link_.empty() || entry_link_[0] == '/' ||
        !split_clean(entry_link_, target_parts)) {
      return invalid("unsafe hardlink archive target");
    }
    const std::string target = join(target_parts, target_parts.size());
    struct stat known{};
    if (fstatat(root_fd_, target.c_str(), &known, AT_SYMLINK_NOFOLLOW) != 0) {
      return {StatusCode::not_found,
              "hardlink archive target is missing: " + target};
    }
    if (!S_ISREG(known.st_mode)) {
      return invalid("hardlink archive target is not a file: " + target);
    }
    Status status = ensure_parent(clean);
    if (!status.ok()) {
      return status;
    }
    const int parent_fd =
        parent.empty()
            ? ::dup(root_fd_)
            : ::openat(root_fd_, parent.c_str(),
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent_fd < 0) {
      return io_error("cannot open extraction parent");
    }
    if (unlinkat(parent_fd, leaf.c_str(), 0) != 0 && errno != ENOENT) {
      ::close(parent_fd);
      return io_error("cannot replace existing archive path");
    }
    if (linkat(root_fd_, target.c_str(), parent_fd, leaf.c_str(), 0) != 0) {
      ::close(parent_fd);
      return io_error("cannot create extracted hardlink");
    }
    ::close(parent_fd);
    ++report_.files;
    return Status::Ok();
  }
  return invalid("unsupported archive entry type");
}

Status TarExtractor::ensure_parent(const std::string &path) {
  std::vector<std::string> parts;
  if (!split_clean(path, parts) || parts.size() < 2) {
    return parts.size() == 1 ? Status::Ok()
                             : invalid("unsafe archive path: " + path);
  }
  for (std::size_t i = 1; i < parts.size(); ++i) {
    const auto prefix = join(parts, i);
    if (mkdirat(root_fd_, prefix.c_str(), 0755) == 0) {
      continue;
    }
    if (errno != EEXIST) {
      return io_error("cannot create extraction parent");
    }
    struct stat known{};
    if (fstatat(root_fd_, prefix.c_str(), &known, AT_SYMLINK_NOFOLLOW) != 0) {
      return io_error("cannot inspect extraction parent");
    }
    if (!S_ISDIR(known.st_mode)) {
      // Never traverse a symlink (or file) while writing: a link placed
      // earlier cannot redirect a later entry outside the root.
      return invalid("archive path traverses a non-directory: " + prefix);
    }
  }
  return Status::Ok();
}

Status TarExtractor::open_clean(const std::string &path) {
  const int fd = ::openat(root_fd_, path.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return io_error("cannot open extracted directory");
  }
  if (fchmod(fd, static_cast<mode_t>(entry_mode_)) != 0) {
    ::close(fd);
    return io_error("cannot set extracted directory mode");
  }
  ::close(fd);
  return Status::Ok();
}

Status TarExtractor::remove_tree(const std::string &path) {
  struct stat known{};
  if (fstatat(root_fd_, path.c_str(), &known, AT_SYMLINK_NOFOLLOW) != 0) {
    return errno == ENOENT
               ? Status{StatusCode::not_found, "archive path is missing"}
               : io_error("cannot inspect archive path");
  }
  if (!S_ISDIR(known.st_mode)) {
    if (unlinkat(root_fd_, path.c_str(), 0) != 0) {
      return io_error("cannot remove archived path");
    }
    return Status::Ok();
  }
  const int fd = ::openat(root_fd_, path.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return io_error("cannot open directory for removal");
  }
  DIR *dir = fdopendir(fd);
  if (!dir) {
    ::close(fd);
    return io_error("cannot inspect directory for removal");
  }
  Status status = Status::Ok();
  while (dirent *entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    status = remove_tree(path + "/" + name);
    if (!status.ok()) {
      break;
    }
  }
  closedir(dir);
  if (!status.ok()) {
    return status;
  }
  if (unlinkat(root_fd_, path.c_str(), AT_REMOVEDIR) != 0) {
    return io_error("cannot remove archived directory");
  }
  return Status::Ok();
}

Status TarExtractor::finish() {
  if (root_fd_ < 0) {
    return {StatusCode::unavailable, "extractor is not open"};
  }
  if (failed_) {
    return {StatusCode::unavailable, "extractor has already failed"};
  }
  if (in_data_) {
    failed_ = true;
    return {StatusCode::invalid_argument, "archive is truncated"};
  }
  for (const char byte : pending_) {
    if (byte != 0) {
      failed_ = true;
      return {StatusCode::invalid_argument,
              "trailing data after end of archive"};
    }
  }
  pending_.clear();
  if (!done_) {
    failed_ = true;
    return {StatusCode::invalid_argument,
            "archive is missing its end-of-archive marker"};
  }
  if (have_long_name_ || have_long_link_) {
    failed_ = true;
    return {StatusCode::invalid_argument,
            "archive ends with an unused long-name entry"};
  }
  // Re-arm for the next layer archive; cumulative bounds and reports stay.
  done_ = false;
  saw_end_ = false;
  return Status::Ok();
}

Status TarExtractor::close() {
  if (entry_fd_ >= 0) {
    ::close(entry_fd_);
    entry_fd_ = -1;
  }
  if (root_fd_ >= 0) {
    ::close(root_fd_);
    root_fd_ = -1;
  }
  pending_.clear();
  return Status::Ok();
}

TarReport TarExtractor::report() const { return report_; }

} // namespace omnimesh

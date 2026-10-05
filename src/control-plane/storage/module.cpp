#include "omnimesh/storage.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace omnimesh {
namespace {
constexpr char kMagic[8] = {'O', 'M', 'N', 'J', 'L', '0', '0', '1'};
constexpr std::size_t kHeaderBytes = 20;
constexpr std::size_t kFrameBytes = 24;
Status io_error(const char *message) {
  return {StatusCode::internal,
          std::string(message) + ": " + std::strerror(errno)};
}
void store(unsigned char *target, std::uint64_t value, unsigned size) {
  for (unsigned i = 0; i < size; ++i) {
    target[i] = static_cast<unsigned char>(value >> (8 * i));
  }
}
std::uint64_t load(const unsigned char *source, unsigned size) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < size; ++i) {
    value |= static_cast<std::uint64_t>(source[i]) << (8 * i);
  }
  return value;
}
Status corrupt() {
  return {StatusCode::invalid_argument,
          "journal corruption or unsupported format; state was preserved"};
}
} // namespace
std::uint64_t record_checksum(std::uint64_t sequence, std::uint8_t type,
                              std::string_view payload) noexcept {
  // Retain the checksum seed of journal v1 for on-disk compatibility.
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](unsigned char byte) {
    hash = (hash ^ byte) * 1099511628211ULL;
  };
  for (unsigned i = 0; i < 8; ++i) {
    mix(static_cast<unsigned char>(sequence >> (8 * i)));
  }
  mix(type);
  for (unsigned char byte : payload) {
    mix(byte);
  }
  return hash;
}
Journal::~Journal() { close(); }
Status Journal::open(const std::string &path) {
  auto status = close();
  if (!status.ok()) {
    return status;
  }
  diagnostics_.clear();
  sequence_ = 0;
  bytes_ = 0;
  faulted_ = false;
  if (path.empty() || path.size() > 4096 ||
      path.find('\0') != std::string::npos) {
    return {StatusCode::invalid_argument, "invalid journal path"};
  }
  const int fd =
      ::open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    return io_error("cannot open journal");
  }
  struct stat info{};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_uid != getuid() || (info.st_mode & 0777) != 0600 ||
      info.st_nlink != 1) {
    ::close(fd);
    return {StatusCode::permission_denied,
            "journal must be a private regular file owned by this user"};
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return {StatusCode::conflict, "journal already has an owner"};
  }
  file_ = fdopen(fd, "r+b");
  if (!file_) {
    ::close(fd);
    return io_error("cannot open journal stream");
  }
  if (info.st_size == 0) {
    unsigned char header[kHeaderBytes]{};
    std::memcpy(header, kMagic, 8);
    store(header + 8, kJournalVersion, 4);
    if (std::fwrite(header, 1, sizeof(header), file_) != sizeof(header)) {
      status = io_error("cannot initialize journal");
    } else {
      status = sync();
    }
    if (status.ok()) {
      const auto slash = path.rfind('/');
      const auto parent = slash == std::string::npos ? "."
                          : slash == 0               ? "/"
                                                     : path.substr(0, slash);
      const int directory =
          ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (directory < 0 || fsync(directory) != 0) {
        status = io_error("cannot sync journal directory");
      }
      if (directory >= 0) {
        ::close(directory);
      }
    }
  }
  if (status.ok()) {
    status = scan(true);
  }
  if (!status.ok()) {
    std::fclose(file_);
    file_ = nullptr;
  }
  return status;
}
Status Journal::scan(bool repair_tail) {
  struct stat info{};
  if (fstat(fileno(file_), &info) != 0) {
    return io_error("cannot inspect journal");
  }
  if (info.st_size > static_cast<off_t>(kMaxJournalBytes)) {
    return {StatusCode::resource_exhausted, "journal exceeds the 16 MiB limit"};
  }
  std::clearerr(file_);
  if (std::fseek(file_, 0, SEEK_SET) != 0) {
    return io_error("cannot seek journal");
  }
  unsigned char header[kHeaderBytes];
  if (std::fread(header, 1, sizeof(header), file_) != sizeof(header)) {
    return std::ferror(file_) ? io_error("cannot read journal header")
                              : corrupt();
  }
  if (std::memcmp(header, kMagic, 8) != 0 ||
      load(header + 8, 4) != kJournalVersion || load(header + 12, 8) != 0) {
    return corrupt();
  }
  std::uint64_t sequence = 0, position = kHeaderBytes;
  bool torn = false;
  while (true) {
    unsigned char frame[kFrameBytes];
    const auto count = std::fread(frame, 1, sizeof(frame), file_);
    if (std::ferror(file_)) {
      return io_error("cannot read journal frame");
    }
    if (count == 0) {
      break;
    }
    if (count != sizeof(frame)) {
      torn = true;
      break;
    }
    if (sequence >= kMaxRecordsPerReplay) {
      return {StatusCode::resource_exhausted, "journal record limit reached"};
    }
    const auto length = load(frame + 8, 4), type = load(frame + 20, 4);
    if (length == 0 || length > kMaxRecordBytes || type > 255 ||
        load(frame, 8) != sequence + 1) {
      return corrupt();
    }
    std::string payload(static_cast<std::size_t>(length), '\0');
    if (std::fread(payload.data(), 1, payload.size(), file_) !=
        payload.size()) {
      if (std::ferror(file_)) {
        return io_error("cannot read journal payload");
      }
      torn = true;
      break;
    }
    if (load(frame + 12, 8) != record_checksum(sequence + 1,
                                               static_cast<std::uint8_t>(type),
                                               payload)) {
      return corrupt();
    }
    ++sequence;
    position += kFrameBytes + length;
  }
  if (torn) {
    if (!repair_tail) {
      return corrupt();
    }
    if (ftruncate(fileno(file_), static_cast<off_t>(position)) != 0 ||
        fsync(fileno(file_)) != 0) {
      return io_error("cannot discard incomplete journal tail");
    }
    if (diagnostics_.size() < 64) {
      diagnostics_.push_back(
          {"$journal", "discarded an incomplete trailing record"});
    }
  }
  sequence_ = sequence;
  bytes_ = position;
  std::clearerr(file_);
  if (std::fseek(file_, static_cast<long>(bytes_), SEEK_SET) != 0) {
    return io_error("cannot seek journal end");
  }
  return Status::Ok();
}
Status Journal::append(std::uint8_t type, const std::string &payload) {
  if (!file_ || faulted_) {
    return {StatusCode::unavailable,
            "journal is closed or has an unresolved I/O failure"};
  }
  if (payload.empty() || payload.size() > kMaxRecordBytes) {
    return {StatusCode::invalid_argument,
            "record payload size is out of bounds"};
  }
  if (sequence_ >= kMaxRecordsPerReplay ||
      bytes_ + kFrameBytes + payload.size() > kMaxJournalBytes) {
    return {StatusCode::resource_exhausted,
            "journal capacity reached; compaction is unsupported"};
  }
  unsigned char frame[kFrameBytes];
  store(frame, sequence_ + 1, 8);
  store(frame + 8, payload.size(), 4);
  store(frame + 12, record_checksum(sequence_ + 1, type, payload), 8);
  store(frame + 20, type, 4);
  if (std::fseek(file_, static_cast<long>(bytes_), SEEK_SET) != 0 ||
      std::fwrite(frame, 1, sizeof(frame), file_) != sizeof(frame) ||
      std::fwrite(payload.data(), 1, payload.size(), file_) != payload.size()) {
    faulted_ = true;
    return io_error("cannot append journal record");
  }
  const auto status = sync();
  if (!status.ok()) {
    faulted_ = true;
    return status;
  }
  ++sequence_;
  bytes_ += kFrameBytes + payload.size();
  return Status::Ok();
}
Status Journal::replay(
    const std::function<void(std::uint8_t, const std::string &)> &apply) {
  if (!file_ || faulted_) {
    return {StatusCode::unavailable,
            "journal is closed or has an unresolved I/O failure"};
  }
  if (!apply) {
    return {StatusCode::invalid_argument, "replay requires a callback"};
  }
  // Verify the entire file before applying any frame. Complete corruption must
  // never recover a deceptively successful prefix with missing reservations.
  auto status = sync();
  if (status.ok()) {
    status = scan(true);
  }
  if (!status.ok()) {
    faulted_ = true;
    return status;
  }
  if (std::fseek(file_, kHeaderBytes, SEEK_SET) != 0) {
    return io_error("cannot rewind journal");
  }
  for (std::uint64_t index = 0; index < sequence_; ++index) {
    unsigned char frame[kFrameBytes];
    if (std::fread(frame, 1, sizeof(frame), file_) != sizeof(frame)) {
      faulted_ = true;
      return io_error("journal changed during replay");
    }
    std::string payload(static_cast<std::size_t>(load(frame + 8, 4)), '\0');
    if (std::fread(payload.data(), 1, payload.size(), file_) !=
        payload.size()) {
      faulted_ = true;
      return io_error("journal changed during replay");
    }
    try {
      apply(static_cast<std::uint8_t>(load(frame + 20, 4)), payload);
    } catch (const std::exception &) {
      std::fseek(file_, static_cast<long>(bytes_), SEEK_SET);
      return {StatusCode::invalid_argument,
              "journal record could not be applied; recovered state must not "
              "be used"};
    }
  }
  return std::fseek(file_, static_cast<long>(bytes_), SEEK_SET) == 0
             ? Status::Ok()
             : io_error("cannot seek journal end");
}
Status Journal::sync() {
  if (!file_) {
    return {StatusCode::unavailable, "journal is not open"};
  }
  if (std::fflush(file_) != 0 || fsync(fileno(file_)) != 0) {
    faulted_ = true;
    return io_error("cannot sync journal");
  }
  return Status::Ok();
}
Status Journal::close() {
  if (!file_) {
    return Status::Ok();
  }
  const auto status = sync();
  const int closed = std::fclose(file_);
  file_ = nullptr;
  return status.ok() && closed != 0 ? io_error("cannot close journal") : status;
}
JournalStats Journal::stats() const { return {sequence_, bytes_, sequence_}; }
const std::vector<Diagnostic> &Journal::diagnostics() const {
  return diagnostics_;
}
} // namespace omnimesh

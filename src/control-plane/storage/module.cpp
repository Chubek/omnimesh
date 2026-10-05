#include "omnimesh/storage.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace omnimesh {
namespace {

constexpr char kMagic[8] = {'O', 'M', 'N', 'J', 'L', '0', '0', '1'};
constexpr std::size_t kHeaderBytes = sizeof(kMagic) + sizeof(std::uint32_t) +
                                     sizeof(std::uint64_t);
// Frame layout, little-endian, fixed width: sequence (u64), payload length
// (u32), checksum (u64), record type (u32), then the payload bytes.
constexpr std::size_t kFrameBytes = sizeof(std::uint64_t) + sizeof(std::uint32_t) +
                                    sizeof(std::uint64_t) + sizeof(std::uint32_t);

Status io_error(const char* what) {
  return {StatusCode::internal, std::string(what) + ": " + std::strerror(errno)};
}

void store_u32(unsigned char* target, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    target[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xffu);
  }
}

void store_u64(unsigned char* target, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    target[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xffu);
  }
}

std::uint32_t load_u32(const unsigned char* source) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(source[i]) << (8 * i);
  }
  return value;
}

std::uint64_t load_u64(const unsigned char* source) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(source[i]) << (8 * i);
  }
  return value;
}

// Distinguishes a clean end of file from a short read of a torn record.
bool read_exactly(std::FILE* file, unsigned char* target, std::size_t size) {
  return std::fread(target, 1, size, file) == size;
}

Status write_exactly(std::FILE* file, const unsigned char* source, std::size_t size) {
  if (std::fwrite(source, 1, size, file) == size) {
    return Status::Ok();
  }
  return io_error("journal write failed");
}

// Rewind to a verified prefix length and drop everything after it, so later
// appends cannot build on bytes that were never committed.
Status truncate_tail(std::FILE* file, std::uint64_t size,
                    std::vector<Diagnostic>& diagnostics) {
  if (::ftruncate(::fileno(file), static_cast<::off_t>(size)) != 0) {
    return io_error("journal truncate failed");
  }
  if (std::fflush(file) != 0) {
    return io_error("journal flush failed");
  }
  diagnostics.push_back({"$journal", "discarded an unverified trailing record"});
  return Status::Ok();
}

} // namespace

std::uint64_t record_checksum(std::uint64_t sequence, std::uint8_t type,
                              std::string_view payload) noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&hash](unsigned char byte) { hash = (hash ^ byte) * 1099511628211ULL; };
  for (int i = 0; i < 8; ++i) {
    mix(static_cast<unsigned char>((sequence >> (8 * i)) & 0xffu));
  }
  mix(type);
  for (const char character : payload) {
    mix(static_cast<unsigned char>(character));
  }
  return hash;
}

Journal::~Journal() { close(); }

Status Journal::open(const std::string& path) {
  close();
  path_ = path;
  diagnostics_.clear();
  sequence_ = 0;
  records_ = 0;
  file_ = std::fopen(path.c_str(), "r+b");
  const bool fresh = file_ == nullptr;
  if (fresh) {
    if (errno != ENOENT) {
      return io_error("cannot open journal");
    }
    file_ = std::fopen(path.c_str(), "w+b");
    if (!file_) {
      return io_error("cannot create journal");
    }
  }
  if (fresh) {
    unsigned char header[kHeaderBytes];
    std::memcpy(header, kMagic, sizeof(kMagic));
    store_u32(header + sizeof(kMagic), kJournalVersion);
    store_u64(header + sizeof(kMagic) + sizeof(std::uint32_t), 0);
    auto status = write_exactly(file_, header, sizeof(header));
    if (status.ok()) {
      status = sync();
    }
    if (!status.ok()) {
      close();
    }
    return status;
  }
  unsigned char header[kHeaderBytes];
  if (!read_exactly(file_, header, sizeof(header)) || std::ferror(file_) ||
      std::memcmp(header, kMagic, sizeof(kMagic)) != 0 ||
      load_u32(header + sizeof(kMagic)) != kJournalVersion) {
    close();
    return {StatusCode::invalid_argument, "journal is not a recognized v1 log"};
  }
  sequence_ = load_u64(header + sizeof(kMagic) + sizeof(std::uint32_t));
  if (std::fseek(file_, 0, SEEK_END) != 0) {
    close();
    return io_error("journal seek failed");
  }
  return Status::Ok();
}

Status Journal::append(std::uint8_t type, const std::string& payload) {
  if (!file_) {
    return {StatusCode::unavailable, "journal is not open"};
  }
  if (payload.empty() || payload.size() > kMaxRecordBytes) {
    return {StatusCode::invalid_argument, "record payload size is out of bounds"};
  }
  const auto status = write_record(type, payload);
  return status.ok() ? sync() : status;
}

Status Journal::write_record(std::uint8_t type, const std::string& payload) {
  const auto sequence = sequence_ + 1;
  unsigned char frame[kFrameBytes];
  store_u64(frame, sequence);
  store_u32(frame + sizeof(std::uint64_t), static_cast<std::uint32_t>(payload.size()));
  store_u64(frame + sizeof(std::uint64_t) + sizeof(std::uint32_t),
            record_checksum(sequence, type, payload));
  store_u32(frame + kFrameBytes - sizeof(std::uint32_t), type);
  const auto start = std::ftell(file_);
  auto status = write_exactly(file_, frame, sizeof(frame));
  if (status.ok()) {
    status = write_exactly(file_, reinterpret_cast<const unsigned char*>(payload.data()), length);
  }
  if (!status.ok()) {
    if (start >= 0) {
      ::ftruncate(::fileno(file_), start);
      std::fflush(file_);
    }
    return status;
  }
  sequence_ = sequence;
  ++records_;
  return Status::Ok();
}

Status Journal::sync() {
  if (!file_) {
    return {StatusCode::unavailable, "journal is not open"};
  }
  if (std::fflush(file_) != 0) {
    return io_error("journal flush failed");
  }
  if (::fsync(::fileno(file_)) != 0) {
    return io_error("journal fsync failed");
  }
  return Status::Ok();
}

Status Journal::replay(const std::function<void(std::uint8_t, const std::string&)>& apply) {
  if (!file_) {
    return {StatusCode::unavailable, "journal is not open"};
  }
  if (std::fseek(file_, 0, SEEK_SET) != 0) {
    return io_error("journal seek failed");
  }
  unsigned char header[kHeaderBytes];
  if (!read_exactly(file_, header, sizeof(header)) || std::ferror(file_) ||
      std::memcmp(header, kMagic, sizeof(kMagic)) != 0 ||
      load_u32(header + sizeof(kMagic)) != kJournalVersion) {
    return {StatusCode::invalid_argument, "journal is not a recognized v1 log"};
  }
  auto expected = load_u64(header + sizeof(kMagic) + sizeof(std::uint32_t));
  std::uint64_t position = kHeaderBytes;
  std::uint64_t replayed = 0;
  while (true) {
    if (++replayed > kMaxRecordsPerReplay) {
      return {StatusCode::resource_exhausted, "journal record limit reached during replay"};
    }
    unsigned char frame[kFrameBytes];
    const auto read = std::fread(frame, 1, sizeof(frame), file_);
    if (read == 0 && std::feof(file_)) {
      break;
    }
    const auto length = load_u32(frame + sizeof(std::uint64_t));
    if (read != sizeof(frame) || std::ferror(file_) || length == 0 ||
        length > kMaxRecordBytes) {
      const auto truncated = truncate_tail(file_, position, diagnostics_);
      return truncated.ok() ? Status::Ok() : truncated;
    }
    std::string payload(length, '\0');
    if (!read_exactly(file_, reinterpret_cast<unsigned char*>(&payload[0]), length) ||
        std::ferror(file_)) {
      const auto truncated = truncate_tail(file_, position, diagnostics_);
      return truncated.ok() ? Status::Ok() : truncated;
    }
    const auto sequence = load_u64(frame);
    const auto checksum = load_u64(frame + sizeof(std::uint64_t) + sizeof(std::uint32_t));
    const auto type =
        static_cast<std::uint8_t>(load_u32(frame + kFrameBytes - sizeof(std::uint32_t)));
    if (sequence != expected + 1 || checksum != record_checksum(sequence, type, payload)) {
      // A gap or mismatch after a verified prefix is treated as a torn tail: the
      // remaining bytes cannot be trusted, so they are discarded rather than applied.
      const auto truncated = truncate_tail(file_, position, diagnostics_);
      return truncated.ok() ? Status::Ok() : truncated;
    }
    apply(type, payload);
    expected = sequence;
    position += kFrameBytes + length;
  }
  sequence_ = expected;
  records_ = expected;
  return Status::Ok();
}

Status Journal::close() {
  if (!file_) {
    return Status::Ok();
  }
  const auto status = sync();
  std::fclose(file_);
  file_ = nullptr;
  return status;
}

JournalStats Journal::stats() const {
  std::uint64_t bytes = 0;
  if (file_) {
    const auto position = std::ftell(file_);
    bytes = position > 0 ? static_cast<std::uint64_t>(position) : 0;
  }
  return {sequence_, bytes, records_};
}

const std::vector<Diagnostic>& Journal::diagnostics() const { return diagnostics_; }

} // namespace omnimesh
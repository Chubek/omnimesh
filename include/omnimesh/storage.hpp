#pragma once

#include "omnimesh/manifest.hpp"
#include <cstdio>
#include <functional>

namespace omnimesh {
inline constexpr std::size_t kMaxRecordBytes = 64 * 1024;
inline constexpr std::uint32_t kJournalVersion = 1;
inline constexpr std::size_t kMaxRecordsPerReplay = 65536;
inline constexpr std::size_t kMaxSnapshotBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxJournalBytes = kMaxSnapshotBytes;
struct JournalStats {
  std::uint64_t sequence{0};
  std::uint64_t bytes{0};
  std::uint64_t records{0};
};
// Single writer, versioned, bounded event journal. A short final frame/payload
// is removed on open. Complete checksum/sequence errors fail closed and
// preserve the file. Fsync precedes acknowledgment; no cryptographic integrity
// is implied. Callers must stop mutation after any I/O error. No compaction is
// implemented.
class Journal {
public:
  Journal() = default;
  ~Journal();
  Journal(const Journal &) = delete;
  Journal &operator=(const Journal &) = delete;
  Status open(const std::string &path);
  Status append(std::uint8_t type, const std::string &payload);
  Status
  replay(const std::function<void(std::uint8_t, const std::string &)> &apply);
  Status close();
  Status sync();
  JournalStats stats() const;
  const std::vector<Diagnostic> &diagnostics() const;

private:
  Status scan(bool repair_tail);
  std::FILE *file_{nullptr};
  std::uint64_t sequence_{0};
  std::uint64_t bytes_{0};
  bool faulted_{false};
  std::vector<Diagnostic> diagnostics_;
};
// Noncryptographic integrity detection; not a signature or authorization check.
std::uint64_t record_checksum(std::uint64_t sequence, std::uint8_t type,
                              std::string_view payload) noexcept;
} // namespace omnimesh

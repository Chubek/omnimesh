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
// is implied. Callers must stop mutation after any I/O error. Compaction
// rewrites the journal from a caller-supplied complete fact set; it is
// crash-atomic and retains exclusive writer ownership through replacement.
// Methods on one instance require external serialization.
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

  // Rewrite this journal so it contains only `records`, atomically. The
  // replacement is fully written and verified in a sibling temporary file before
  // a single atomic rename swaps it in, so an interruption at any point leaves
  // either the original or complete replacement authoritative. Sequence numbers
  // restart at 1, so the replacement must carry every fact needed for recovery.
  //
  // Both inodes stay locked across rename; the installed handle is transferred
  // without reopening. Callers must serialize their own state mutations while
  // deriving the complete fact set. An I/O failure requires close and recovery.
  Status compact(const std::vector<std::pair<std::uint8_t, std::string>> &records);

private:
  Status attach(int fd);
  Status scan(bool repair_tail);
  std::FILE *file_{nullptr};
  std::string path_;
  std::uint64_t sequence_{0};
  std::uint64_t bytes_{0};
  bool faulted_{false};
  std::vector<Diagnostic> diagnostics_;
};
// Noncryptographic integrity detection; not a signature or authorization check.
std::uint64_t record_checksum(std::uint64_t sequence, std::uint8_t type,
                              std::string_view payload) noexcept;
} // namespace omnimesh

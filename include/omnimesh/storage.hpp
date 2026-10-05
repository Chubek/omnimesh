#pragma once

+#include "omnimesh/allocator.hpp"
+#include "omnimesh/orchestrator.hpp"
+
+#include <cstdio>
+
+namespace omnimesh {
+
+inline constexpr std::size_t kMaxRecordBytes = 64 * 1024;
+inline constexpr std::uint32_t kJournalVersion = 1;
+inline constexpr std::size_t kMaxRecordsPerReplay = 65536;
+inline constexpr std::size_t kMaxSnapshotBytes = 16 * 1024 * 1024;
+
+struct JournalStats {
+  std::uint64_t sequence{0};
+  std::uint64_t bytes{0};
+  std::uint64_t records{0};
+};
+
+// Append-only durable log of authoritative control-plane facts.
+//
+// Every record is length-prefixed and checksummed. A torn tail from an
+// interrupted write is truncated on open, so a partially written record is
+// never interpreted as authoritative. Records are committed with fsync before
+// append returns, so a record that append accepted survives process death.
+//
+// The allocator and controller stay authoritative in memory; this log is their
+// recovery input, not a second source of truth.
+class Journal {
+public:
+  Journal() = default;
+  ~Journal();
+  Journal(const Journal&) = delete;
+  Journal& operator=(const Journal&) = delete;
+
+  Status open(const std::string& path);
+  Status append(std::uint8_t type, const std::string& payload);
+  Status replay(const std::function<void(std::uint8_t, const std::string&)>& apply);
+  Status close();
+  JournalStats stats() const;
+  const std::vector<Diagnostic>& diagnostics() const;
+
+private:
+  Status write_record(std::uint8_t type, const std::string& payload);
+  Status sync();
+
+  std::FILE* file_{nullptr};
+  std::string path_;
+  std::uint64_t sequence_{0};
+  std::uint64_t records_{0};
+  std::vector<Diagnostic> diagnostics_;
+};
+
+// FNV-1a, matching the checksum domain used by record framing. Chosen for
+// dependency-free integrity detection of torn writes, not for security.
+std::uint64_t record_checksum(std::uint64_t sequence, std::uint8_t type,
                             std::string_view payload) noexcept;
+
+} // namespace omnimesh
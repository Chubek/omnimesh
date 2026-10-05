#pragma once

#include "omnimesh/manifest.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace omnimesh {

// Single blobs are bounded so hashing and transfers stay in explicit limits;
// the spool directory additionally carries a total capacity quota.
inline constexpr std::uint64_t kMaxArtifactBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxArtifactCount = 1024;
inline constexpr std::uint64_t kDefaultSpoolCapacityBytes =
    256ULL * 1024ULL * 1024ULL;

// Digests are strict `sha256:<64 lowercase hex>`, matching image references.
// Only validated digests ever touch the filesystem, so names cannot traverse.
bool valid_artifact_digest(const std::string &digest) noexcept;

struct ArtifactInfo {
  std::string digest;
  std::uint64_t size{0};
  std::string tenant;
};

struct SpoolOptions {
  std::string directory;
  std::uint64_t capacity_bytes{kDefaultSpoolCapacityBytes};
  std::size_t max_artifacts{kMaxArtifactCount};
};

struct SpoolStats {
  std::uint64_t blobs{0};
  std::uint64_t bytes{0};
  std::uint64_t capacity_bytes{0};
  std::uint64_t max_artifacts{0};
};

struct CollectReport {
  std::uint64_t removed{0};
  std::uint64_t reclaimed_bytes{0};
};

struct FetchOptions {
  std::uint64_t max_bytes{kMaxArtifactBytes};
  std::function<bool()> cancelled;
};

// Content-addressed local spool. Blobs are immutable once published: `put`
// hashes while streaming and publishes atomically, and `fetch` re-hashes
// while reading and fails closed on mismatch, preserving the stored blob.
// Tenants are recorded at publish time and enforced on fetch; this is local
// discretionary labeling, not authenticated isolation. One open spool holds
// an exclusive directory lock against other processes; threads share a mutex.
// Opening reconciles the directory: unknown files fail the open, missing
// blobs are dropped from the index, and an unreadable index is rebuilt by
// re-hashing (which resets tenant labels to empty).
class ArtifactSpool {
public:
  ArtifactSpool() = default;
  ~ArtifactSpool();
  ArtifactSpool(const ArtifactSpool &) = delete;
  ArtifactSpool &operator=(const ArtifactSpool &) = delete;

  Status open(const SpoolOptions &options,
              const std::function<bool()> &cancelled = {});
  Status close();

  Status put(const std::string &path, const std::string &tenant,
             ArtifactInfo &info);
  Status put_bytes(std::string_view data, const std::string &tenant,
                   ArtifactInfo &info);
  Status fetch(const std::string &digest, const std::string &tenant,
               const std::string &out_path, const FetchOptions &options = {});
  Status fetch_bytes(const std::string &digest, const std::string &tenant,
                     std::string &data);
  Status inspect(const std::string &digest, ArtifactInfo &info) const;
  std::vector<ArtifactInfo> list() const;
  Status collect(const std::set<std::string> &keep, CollectReport &report);
  SpoolStats stats() const;
  std::vector<Diagnostic> diagnostics() const;

private:
  Status save_index();
  Status reconcile(const std::function<bool()> &cancelled);
  std::string blob_path(const std::string &hex) const;

  mutable std::mutex mutex_;
  bool open_{false};
  int lock_fd_{-1};
  SpoolOptions options_;
  std::map<std::string, ArtifactInfo> index_; // keyed by full digest
  std::vector<Diagnostic> diagnostics_;
};

} // namespace omnimesh

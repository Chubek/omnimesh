#include "omnimesh/artifacts.hpp"
#include "omnimesh/sha256.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace omnimesh;

namespace {

struct Sandbox {
  std::string root;
  Sandbox() {
    char path[] = "/tmp/omnimesh-artifacts-XXXXXX";
    CHECK(mkdtemp(path) != nullptr);
    root = path;
  }
  ~Sandbox() { std::filesystem::remove_all(root); }
  std::string spool() const { return root + "/spool"; }
  SpoolOptions options() const {
    SpoolOptions result;
    result.directory = spool();
    return result;
  }
  void write(const std::string &path, const std::string &data) {
    std::ofstream file(path, std::ios::binary);
    CHECK(static_cast<bool>(file));
    file << data;
    file.close();
    CHECK(static_cast<bool>(file));
  }
  void corrupt_blob(const std::string &digest) {
    const auto path = spool() + "/blobs/" + digest.substr(7);
    CHECK(chmod(path.c_str(), 0600) == 0);
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    CHECK(static_cast<bool>(file));
    char byte = 0;
    file.seekg(0);
    file.get(byte);
    file.seekp(0);
    byte = static_cast<char>(byte ^ 0x01);
    file.put(byte);
    file.close();
    CHECK(chmod(path.c_str(), 0400) == 0);
  }
};

void sha256_vectors() {
  CHECK(Sha256::hexdigest("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(Sha256::hexdigest("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(Sha256::hexdigest(
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // Incremental updates across block boundaries match the one-shot hash.
  Sha256 hash;
  hash.update("a");
  hash.update("bc");
  CHECK(Sha256::hex(hash.finish()) == Sha256::hexdigest("abc"));
  Sha256 blocks;
  const std::string million(1000000, 'a');
  for (std::size_t i = 0; i < million.size(); i += 7) {
    blocks.update(million.data() + i, std::min<std::size_t>(7, million.size() - i));
  }
  CHECK(Sha256::hex(blocks.finish()) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  CHECK(!valid_artifact_digest("sha256:xyz"));
  CHECK(!valid_artifact_digest("SHA256:" + std::string(64, 'a')));
  CHECK(!valid_artifact_digest("sha256:" + std::string(63, 'a')));
  CHECK(!valid_artifact_digest("sha256:" + std::string(64, 'A')));
  CHECK(!valid_artifact_digest("sha256:" + std::string(64, 'a') + "b"));
  CHECK(valid_artifact_digest("sha256:" + std::string(64, 'a')));
  CHECK(!valid_artifact_digest("../../etc/passwd"));
}

void round_trip() {
  Sandbox sandbox;
  ArtifactSpool spool;
  CHECK(spool.open(sandbox.options()).ok());
  ArtifactSpool duplicate;
  CHECK(duplicate.open(sandbox.options()).code == StatusCode::conflict);
  const std::string payload = "hello spool";
  ArtifactInfo info;
  CHECK(spool.put_bytes(payload, "local", info).ok());
  CHECK(info.digest == "sha256:" + Sha256::hexdigest(payload));
  CHECK(info.size == payload.size());
  CHECK(info.tenant == "local");
  ArtifactInfo seen;
  CHECK(spool.inspect(info.digest, seen).ok());
  CHECK(seen.size == payload.size() && seen.tenant == "local");
  CHECK(spool.inspect("sha256:" + std::string(64, 'b'), seen).code ==
        StatusCode::not_found);
  CHECK(spool.inspect("nope", seen).code == StatusCode::invalid_argument);
  std::string fetched;
  CHECK(spool.fetch_bytes(info.digest, "local", fetched).ok());
  CHECK(fetched == payload);
  CHECK(spool.fetch_bytes(info.digest, "other", fetched).code ==
        StatusCode::permission_denied);
  const auto listed = spool.list();
  CHECK(listed.size() == 1 && listed[0].digest == info.digest);
  const auto stats = spool.stats();
  CHECK(stats.blobs == 1 && stats.bytes == payload.size());
  CHECK(stats.capacity_bytes == kDefaultSpoolCapacityBytes);
  // Re-publishing identical bytes is idempotent and can relabel the tenant.
  ArtifactInfo relabeled;
  CHECK(spool.put_bytes(payload, "other", relabeled).ok());
  CHECK(relabeled.digest == info.digest);
  CHECK(spool.list().size() == 1);
  CHECK(spool.fetch_bytes(info.digest, "other", fetched).ok());
  CHECK(spool.fetch_bytes(info.digest, "local", fetched).code ==
        StatusCode::permission_denied);
  CHECK(spool.close().ok());
  CHECK(spool.list().empty());
}

void file_round_trip() {
  Sandbox sandbox;
  sandbox.write(sandbox.root + "/input.bin", std::string("\x00\xff binary \n", 11));
  ArtifactSpool spool;
  CHECK(spool.open(sandbox.options()).ok());
  ArtifactInfo info;
  CHECK(spool.put(sandbox.root + "/input.bin", "local", info).ok());
  const auto out = sandbox.root + "/output.bin";
  CHECK(spool.fetch(info.digest, "local", out).ok());
  std::ifstream left(sandbox.root + "/input.bin", std::ios::binary);
  std::ifstream right(out, std::ios::binary);
  CHECK(std::string(std::istreambuf_iterator<char>(left),
                    std::istreambuf_iterator<char>()) ==
        std::string(std::istreambuf_iterator<char>(right),
                    std::istreambuf_iterator<char>()));
  // Fetching again replaces the output file.
  CHECK(spool.fetch(info.digest, "local", out).ok());
  CHECK(spool.fetch("sha256:" + std::string(64, 'c'), "local", out).code ==
        StatusCode::not_found);
  CHECK(spool.put(sandbox.root + "/missing", "local", info).code ==
        StatusCode::internal);
  CHECK(spool.put(sandbox.root, "local", info).code ==
        StatusCode::invalid_argument);
  CHECK(spool.close().ok());
}

void tampering_fails_closed() {
  Sandbox sandbox;
  ArtifactSpool spool;
  CHECK(spool.open(sandbox.options()).ok());
  ArtifactInfo info;
  CHECK(spool.put_bytes("tamper me", "local", info).ok());
  CHECK(spool.close().ok());
  sandbox.corrupt_blob(info.digest);
  // With the index lost, open must re-hash and reject the tampered blob
  // instead of serving it.
  CHECK(std::filesystem::remove(sandbox.spool() + "/index.json"));
  CHECK(spool.open(sandbox.options()).code == StatusCode::invalid_argument);
  // The corrupt blob is preserved for inspection, not served or deleted.
  CHECK(std::filesystem::exists(sandbox.spool() + "/blobs/" +
                                info.digest.substr(7)));
}

void fetch_detects_tampering() {
  Sandbox sandbox;
  ArtifactSpool spool;
  CHECK(spool.open(sandbox.options()).ok());
  ArtifactInfo info;
  CHECK(spool.put_bytes("fetch me", "local", info).ok());
  // Close releases the lock so the test can tamper out of band, then reopen
  // with the index intact to exercise fetch-time verification.
  CHECK(spool.close().ok());
  ArtifactSpool tampered;
  CHECK(tampered.open(sandbox.options()).ok());
  // Tamper without touching the index: chmod is allowed for the owner, and
  // the index still claims the original digest.
  const auto path = sandbox.spool() + "/blobs/" + info.digest.substr(7);
  CHECK(chmod(path.c_str(), 0600) == 0);
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    CHECK(static_cast<bool>(file));
    file.seekp(0);
    file.put('X');
  }
  CHECK(chmod(path.c_str(), 0400) == 0);
  std::string fetched;
  CHECK(tampered.fetch_bytes(info.digest, "local", fetched).code ==
        StatusCode::invalid_argument);
  CHECK(tampered.fetch(info.digest, "local", sandbox.root + "/out").code ==
        StatusCode::invalid_argument);
  CHECK(!std::filesystem::exists(sandbox.root + "/out"));
  CHECK(!std::filesystem::exists(sandbox.root + "/out.omnimesh-part"));
  CHECK(std::filesystem::exists(path)); // Stored blob preserved.
  CHECK(tampered.close().ok());
}

void quotas_and_bounds() {
  Sandbox sandbox;
  SpoolOptions tiny = sandbox.options();
  tiny.capacity_bytes = 10;
  tiny.max_artifacts = 2;
  ArtifactSpool spool;
  CHECK(spool.open(tiny).ok());
  ArtifactInfo info;
  CHECK(spool.put_bytes("12345", "local", info).ok());
  CHECK(spool.put_bytes("67890abc", "local", info).code ==
        StatusCode::resource_exhausted);
  CHECK(spool.put_bytes("12", "local", info).ok());
  CHECK(spool.put_bytes("x", "local", info).code ==
        StatusCode::resource_exhausted);
  CHECK(spool.put_bytes(std::string(kMaxArtifactBytes + 1, 'z'), "local",
                        info)
            .code == StatusCode::invalid_argument);
  CHECK(spool.put_bytes("data", "", info).code ==
        StatusCode::invalid_argument);
  SpoolOptions bad = sandbox.options();
  bad.capacity_bytes = 0;
  ArtifactSpool rejected;
  CHECK(rejected.open(bad).code == StatusCode::invalid_argument);
  CHECK(spool.close().ok());
}

void garbage_collection() {
  Sandbox sandbox;
  ArtifactSpool spool;
  CHECK(spool.open(sandbox.options()).ok());
  ArtifactInfo first, second, third;
  CHECK(spool.put_bytes("first", "local", first).ok());
  CHECK(spool.put_bytes("second", "local", second).ok());
  CHECK(spool.put_bytes("third", "local", third).ok());
  CollectReport report;
  CHECK(spool.collect({"bogus"}, report).code == StatusCode::invalid_argument);
  CHECK(spool.collect({first.digest, third.digest}, report).ok());
  CHECK(report.removed == 1);
  CHECK(report.reclaimed_bytes == std::string("second").size());
  CHECK(spool.list().size() == 2);
  ArtifactInfo seen;
  CHECK(spool.inspect(second.digest, seen).code == StatusCode::not_found);
  std::string fetched;
  CHECK(spool.fetch_bytes(first.digest, "local", fetched).ok());
  CHECK(fetched == "first");
  // Unknown keeps are already gone, so collection simply removes the rest.
  CHECK(spool.collect({"sha256:" + std::string(64, '0')}, report).ok());
  CHECK(report.removed == 2 && spool.list().empty());
  CHECK(spool.close().ok());
}

void recovery_rebuilds_index() {
  Sandbox sandbox;
  ArtifactSpool spool;
  CHECK(spool.open(sandbox.options()).ok());
  ArtifactInfo info;
  CHECK(spool.put_bytes("recoverable", "local", info).ok());
  CHECK(spool.close().ok());
  CHECK(std::filesystem::remove(sandbox.spool() + "/index.json"));
  ArtifactSpool rebuilt;
  CHECK(rebuilt.open(sandbox.options()).ok());
  ArtifactInfo seen;
  CHECK(rebuilt.inspect(info.digest, seen).ok());
  CHECK(seen.size == info.size && seen.tenant.empty());
  std::string fetched;
  CHECK(rebuilt.fetch_bytes(info.digest, "local", fetched).code ==
        StatusCode::permission_denied);
  // Re-publishing identical bytes restores the tenant label.
  ArtifactInfo relabeled;
  CHECK(rebuilt.put_bytes("recoverable", "local", relabeled).ok());
  CHECK(rebuilt.fetch_bytes(info.digest, "local", fetched).ok());
  CHECK(fetched == "recoverable");
  CHECK(!rebuilt.diagnostics().empty());
  CHECK(rebuilt.close().ok());
}

void rejects_foreign_files() {
  Sandbox sandbox;
  ArtifactSpool spool;
  CHECK(spool.open(sandbox.options()).ok());
  CHECK(spool.close().ok());
  std::ofstream(sandbox.spool() + "/blobs/stray") << "x";
  ArtifactSpool foreign;
  CHECK(foreign.open(sandbox.options()).code == StatusCode::invalid_argument);
}

} // namespace

int main() {
  return test::run([] {
    sha256_vectors();
    round_trip();
    file_round_trip();
    tampering_fails_closed();
    fetch_detects_tampering();
    quotas_and_bounds();
    garbage_collection();
    recovery_rebuilds_index();
    rejects_foreign_files();
  });
}

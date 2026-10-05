#include "omnimesh/gzip.hpp"
#include "omnimesh/images.hpp"
#include "omnimesh/sha256.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

using namespace omnimesh;

namespace {

// Frozen gzip members generated with Python's gzip (mtime=0) and verified
// there; they exercise empty, fixed and dynamic Huffman blocks, file names
// and concatenated members.
const std::vector<unsigned char> kGzipEmpty = {
    31, 139, 8, 0, 0, 0, 0, 0, 2, 255, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0};
const std::vector<unsigned char> kGzipHello = {
    31,  139, 8,   0,   0,   0,   0,   0,   2,   255, 203, 72,  205, 201, 201,
    87,  40,  207, 47,  202, 73,  225, 2,   0,   45,  59,  8,   175, 12,  0,
    0,   0};
const std::vector<unsigned char> kGzipPattern = {
    31,  139, 8,   0,   0,   0,   0,   0,   2,   255, 237, 202, 71,  1,
    128, 48,  16,  69,  65,  43,  95,  1,   106, 98,  128, 146, 208, 217,
    16,  8,   77,  61,  136, 224, 248, 206, 51,  174, 243, 90,  115, 95,
    143, 170, 146, 157, 139, 130, 93,  26,  242, 28,  55,  217, 225, 147,
    246, 143, 167, 242, 185, 213, 88,  91,  200, 145, 201, 100, 50,  153,
    76,  38,  147, 201, 255, 229, 23,  62,  240, 145, 249, 140, 10,  0,
    0};
const std::vector<unsigned char> kGzipBinary = {
    31,  139, 8,   0,   0,   0,   0,   0,   2,   255, 99,  96,  100, 98,
    102, 97,  101, 99,  231, 224, 228, 226, 230, 225, 229, 227, 23,  16,
    20,  18,  22,  17,  21,  19,  151, 144, 148, 146, 150, 145, 149, 147,
    87,  80,  84,  82,  86,  81,  85,  83,  215, 208, 212, 210, 214, 209,
    213, 211, 55,  48,  52,  50,  54,  49,  53,  51,  183, 176, 180, 178,
    182, 177, 181, 179, 119, 112, 116, 114, 118, 113, 117, 115, 247, 240,
    244, 242, 246, 241, 245, 243, 15,  8,   12,  10,  14,  9,   13,  11,
    143, 136, 140, 138, 142, 137, 141, 139, 79,  72,  76,  74,  78,  73,
    77,  75,  207, 200, 204, 202, 206, 201, 205, 203, 47,  40,  44,  42,
    46,  41,  45,  43,  175, 168, 172, 170, 174, 169, 173, 171, 111, 104,
    108, 106, 110, 105, 109, 107, 239, 232, 236, 234, 238, 233, 237, 235,
    159, 48,  113, 210, 228, 41,  83,  167, 77,  159, 49,  115, 214, 236,
    57,  115, 231, 205, 95,  176, 112, 209, 226, 37,  75,  151, 45,  95,
    177, 114, 213, 234, 53,  107, 215, 173, 223, 176, 113, 211, 230, 45,
    91,  183, 109, 223, 177, 115, 215, 238, 61,  123, 247, 237, 63,  112,
    240, 208, 225, 35,  71,  143, 29,  63,  113, 242, 212, 233, 51,  103,
    207, 157, 191, 112, 241, 210, 229, 43,  87,  175, 93,  191, 113, 243,
    214, 237, 59,  119, 239, 221, 127, 240, 240, 209, 227, 39,  79,  159,
    61,  127, 241, 242, 213, 235, 55,  111, 223, 189, 255, 240, 241, 211,
    231, 47,  95,  191, 125, 255, 241, 243, 215, 239, 63,  127, 255, 253,
    103, 24,  245, 255, 168, 255, 71,  253, 63,  234, 255, 81,  255, 143,
    250, 127, 4,   250, 31,  0,   88,  221, 94,  159, 0,   8,   0,   0};
const std::vector<unsigned char> kGzipFname = {
    31, 139, 8,   8,   0,   0,   0,   0,   2,   255, 104, 101, 108, 108,
    111, 46,  116, 120, 116, 0,   203, 75,  204, 77,  77,  81,  72,  206,
    207, 43,  73,  205, 43,  1,   0,   41,  249, 72,  127, 13,  0,   0,
    0};
const std::vector<unsigned char> kGzipConcat = {
    31, 139, 8,  0, 0, 0, 0, 0, 2, 255, 75,  203, 44, 42, 46, 1, 0,
    87, 238, 113, 146, 5, 0, 0, 0, 31, 139, 8,  0,   0,  0,  0,  0,
    2,  255, 43, 78, 77, 206, 207, 75, 1, 0,  105, 17, 31, 182, 6, 0,
    0,  0};
// Frozen gzip of a two-file ustar archive (g.txt, sub/dir/f.txt), generated
// the same way. Plain SHA-256 51155a88... and blob a1673b6b... are asserted.
const std::vector<unsigned char> kGzipLayer = {
    31,  139, 8,   0,   0,   0,   0,   0,   2,   255, 237, 212, 65,  10,
    131, 48,  16,  133, 225, 89,  247, 20,  158, 160, 78,  172, 36,  231,
    177, 36,  138, 80,  138, 36,  17,  218, 158, 190, 193, 141, 180, 155,
    174, 20,  138, 255, 183, 121, 195, 108, 102, 245, 102, 56,  231, 71,
    150, 109, 105, 97,  219, 118, 201, 226, 59,  85,  141, 93,  231, 101,
    111, 221, 197, 72,  165, 178, 131, 57,  229, 46,  150, 147, 114, 76,
    195, 107, 156, 166, 224, 171, 91,  247, 12,  241, 36,  56,  152, 52,
    95,  107, 63,  198, 186, 223, 240, 15,  252, 236, 191, 186, 207, 254,
    27,  109, 156, 163, 255, 123, 184, 135, 148, 131, 167, 248, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   255, 234, 13,  7,
    105, 65,  6,   0,   40,  0,   0};

struct Sandbox {
  std::string root;
  Sandbox() {
    char path[] = "/tmp/omnimesh-images-XXXXXX";
    CHECK(mkdtemp(path) != nullptr);
    root = path;
  }
  ~Sandbox() { std::filesystem::remove_all(root); }
  void write(const std::string &path, const std::string &data) {
    std::ofstream file(path, std::ios::binary);
    CHECK(static_cast<bool>(file));
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    CHECK(static_cast<bool>(file));
  }
  void write_bytes(const std::string &path,
                   const std::vector<unsigned char> &data) {
    std::ofstream file(path, std::ios::binary);
    CHECK(static_cast<bool>(file));
    file.write(reinterpret_cast<const char *>(data.data()),
               static_cast<std::streamsize>(data.size()));
    file.close();
    CHECK(static_cast<bool>(file));
  }
  std::string read(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    CHECK(static_cast<bool>(file));
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
  }
};

// Minimal ustar builder with checksums the extractor verifies.
struct TarBuilder {
  std::string bytes;
  void add(const std::string &name, char type, const std::string &link,
           unsigned mode, const std::string &data) {
    unsigned char header[512]{};
    const bool raw = type == 'L' || type == 'K';
    const std::string header_name = raw ? "././@LongLink" : name;
    std::string prefix;
    std::string base = header_name;
    if (!raw && base.size() >= 100) {
      // USTAR prefix split for long paths.
      CHECK(base.size() < 256);
      base = header_name.substr(header_name.size() - 99);
      prefix = header_name.substr(0, header_name.size() - 100);
      CHECK(prefix.size() <= 155);
    }
    CHECK(base.size() < 100 && link.size() < 100);
    std::memcpy(header, base.data(), base.size());
    std::memcpy(header + 345, prefix.data(), prefix.size());
    std::snprintf(reinterpret_cast<char *>(header + 100), 8, "%07o", mode);
    std::snprintf(reinterpret_cast<char *>(header + 124), 12, "%011o",
                  static_cast<unsigned>(data.size()));
    std::memcpy(header + 148, "        ", 8);
    header[156] = static_cast<unsigned char>(type);
    std::memcpy(header + 157, link.data(), link.size());
    std::memcpy(header + 257, "ustar", 5);
    std::memcpy(header + 263, "00", 2);
    unsigned sum = 0;
    for (const auto byte : header) {
      sum += byte;
    }
    std::snprintf(reinterpret_cast<char *>(header + 148), 8, "%06o", sum);
    header[154] = '\0';
    header[155] = ' ';
    bytes.append(reinterpret_cast<const char *>(header), sizeof(header));
    bytes += data;
    const auto pad = (512 - (data.size() % 512)) % 512;
    bytes.append(pad, '\0');
  }
  void file(const std::string &name, const std::string &data,
            unsigned mode = 0644) {
    add(name, '0', "", mode, data);
  }
  void dir(const std::string &name, unsigned mode = 0755) {
    add(name, '5', "", mode, "");
  }
  void end() { bytes.append(1024, '\0'); }
};

std::string gunzip_all(const std::vector<unsigned char> &input,
                       std::size_t stride, std::size_t out_chunk,
                       std::uint64_t max_output = 1ULL << 30) {
  std::size_t pos = 0;
  GzipReader reader(
      [&](unsigned char *buffer, std::size_t capacity) -> ssize_t {
        if (pos >= input.size()) {
          return 0;
        }
        std::size_t take = std::min(capacity, stride);
        take = std::min(take, input.size() - pos);
        std::memcpy(buffer, input.data() + pos, take);
        pos += take;
        return static_cast<ssize_t>(take);
      },
      max_output);
  std::string out;
  std::vector<unsigned char> chunk(out_chunk);
  while (true) {
    std::size_t got = 0;
    CHECK(reader.read(chunk.data(), chunk.size(), got).ok());
    out.append(reinterpret_cast<const char *>(chunk.data()), got);
    if (got == 0) {
      break;
    }
  }
  return out;
}

void gzip_vectors() {
  const std::string pattern =
      "The quick brown fox jumps over the lazy dog. ";
  std::string fox;
  for (int i = 0; i < 60; ++i) {
    fox += pattern;
  }
  std::string binary;
  for (int i = 0; i < 8; ++i) {
    for (int b = 0; b < 256; ++b) {
      binary.push_back(static_cast<char>(b));
    }
  }
  const std::vector<std::pair<std::vector<unsigned char>, std::string>> cases = {
      {kGzipEmpty, ""},
      {kGzipHello, "hello world\n"},
      {kGzipPattern, fox},
      {kGzipBinary, binary},
      {kGzipFname, "named content"},
      {kGzipConcat, "firstsecond"},
  };
  for (const auto &entry : cases) {
    for (const auto stride : {1, 7, 4096}) {
      CHECK(gunzip_all(entry.first, stride, 7) == entry.second);
    }
  }
  auto expect_throw = [](std::function<void()> run) {
    try {
      run();
    } catch (...) {
      return true;
    }
    return false;
  };
  // Corrupt inputs fail closed.
  auto broken_magic = kGzipHello;
  broken_magic[0] = 0x1e;
  for (const auto stride : {1, 4096}) {
    bool failed = false;
    try {
      std::size_t pos = 0;
      GzipReader reader(
          [&](unsigned char *buffer, std::size_t capacity) -> ssize_t {
            if (pos >= broken_magic.size()) {
              return 0;
            }
            const std::size_t take =
                std::min({capacity, static_cast<std::size_t>(stride),
                          broken_magic.size() - pos});
            std::memcpy(buffer, broken_magic.data() + pos, take);
            pos += take;
            return static_cast<ssize_t>(take);
          },
          1ULL << 30);
      std::vector<unsigned char> chunk(64);
      while (true) {
        std::size_t got = 0;
        const auto status = reader.read(chunk.data(), chunk.size(), got);
        if (!status.ok()) {
          failed = true;
          break;
        }
        if (got == 0) {
          break;
        }
      }
    } catch (...) {
      failed = true;
    }
    CHECK(failed);
  }
  auto bad_crc = kGzipHello;
  bad_crc[bad_crc.size() - 8] ^= 0x01; // Flip a trailer CRC byte.
  CHECK(expect_throw([&] { (void)gunzip_all(bad_crc, 64, 64); }));
  auto truncated = kGzipPattern;
  truncated.resize(truncated.size() - 10);
  CHECK(expect_throw([&] { (void)gunzip_all(truncated, 64, 64); }));
  // Output bound is enforced incrementally.
  {
    std::size_t pos = 0;
    GzipReader reader(
        [&](unsigned char *buffer, std::size_t capacity) -> ssize_t {
          if (pos >= kGzipHello.size()) {
            return 0;
          }
          const std::size_t take = std::min(capacity, kGzipHello.size() - pos);
          std::memcpy(buffer, kGzipHello.data() + pos, take);
          pos += take;
          return static_cast<ssize_t>(take);
        },
        5);
    std::vector<unsigned char> chunk(64);
    bool bounded = false;
    while (true) {
      std::size_t got = 0;
      const auto status = reader.read(chunk.data(), chunk.size(), got);
      if (!status.ok()) {
        bounded = status.code == StatusCode::resource_exhausted;
        break;
      }
      if (got == 0) {
        break;
      }
    }
    CHECK(bounded);
  }
}

void tar_basics() {
  Sandbox sandbox;
  const auto root = sandbox.root + "/root";
  CHECK(mkdir(root.c_str(), 0755) == 0);
  TarBuilder builder;
  builder.dir("etc");
  builder.file("etc/hostname", "layer1\n");
  builder.dir("app");
  builder.file("app/run", "v1\n", 0755);
  builder.add("link", '2', "app/run", 0777, "");
  builder.end();
  TarExtractor tar;
  CHECK(tar.open(root, {}).ok());
  // Feed in odd-sized chunks to exercise the streaming parser.
  for (std::size_t at = 0; at < builder.bytes.size();) {
    const std::size_t take = std::min<std::size_t>(7, builder.bytes.size() - at);
    CHECK(tar
              .write(reinterpret_cast<const unsigned char *>(builder.bytes.data() +
                                                             at),
                     take)
              .ok());
    at += take;
  }
  CHECK(tar.finish().ok());
  const auto report = tar.report();
  CHECK(report.files == 2 && report.directories == 2 && report.symlinks == 1);
  CHECK(sandbox.read(root + "/etc/hostname") == "layer1\n");
  CHECK(sandbox.read(root + "/app/run") == "v1\n");
  struct stat info{};
  CHECK(stat((root + "/app/run").c_str(), &info) == 0);
  CHECK((info.st_mode & 0777) == 0755);
  char target[64]{};
  CHECK(readlink((root + "/link").c_str(), target, sizeof(target) - 1) > 0);
  CHECK(std::string(target) == "app/run");
  CHECK(tar.close().ok());
}

void tar_traversal_rejected() {
  for (const std::string &name :
       {std::string("../evil"), std::string("/absolute"),
        std::string("a/../../evil"), std::string("a//b")}) {
    Sandbox sandbox;
    const auto root = sandbox.root + "/root";
    CHECK(mkdir(root.c_str(), 0755) == 0);
    TarBuilder builder;
    builder.file(name, "x");
    TarExtractor tar;
    CHECK(tar.open(root, {}).ok());
    CHECK(!tar.write(builder.bytes).ok());
    CHECK(!std::filesystem::exists(sandbox.root + "/root/evil"));
  }
  // A symlink can be created, but later entries cannot traverse it.
  {
    Sandbox sandbox;
    const auto root = sandbox.root + "/root";
    CHECK(mkdir(root.c_str(), 0755) == 0);
    TarBuilder builder;
    builder.add("link", '2', "/etc", 0777, "");
    builder.file("link/evil", "x");
    builder.end();
    TarExtractor tar;
    CHECK(tar.open(root, {}).ok());
    CHECK(!tar.write(builder.bytes).ok());
    CHECK(!std::filesystem::exists("/etc/evil-tar-probe"));
  }
  // Absolute symlink targets are created (standard in images) and left
  // alone; only traversal is refused.
  {
    Sandbox sandbox;
    const auto root = sandbox.root + "/root";
    CHECK(mkdir(root.c_str(), 0755) == 0);
    TarBuilder builder;
    builder.add("abs", '2', "/etc/hostname", 0777, "");
    builder.end();
    TarExtractor tar;
    CHECK(tar.open(root, {}).ok());
    CHECK(tar.write(builder.bytes).ok());
    CHECK(tar.finish().ok());
  }
}

void tar_hardlinks() {
  Sandbox sandbox;
  const auto root = sandbox.root + "/root";
  CHECK(mkdir(root.c_str(), 0755) == 0);
  TarBuilder builder;
  builder.file("orig", "shared\n");
  builder.add("hard", '1', "orig", 0644, "");
  builder.end();
  TarExtractor tar;
  CHECK(tar.open(root, {}).ok());
  CHECK(tar.write(builder.bytes).ok());
  CHECK(tar.finish().ok());
  struct stat first{}, second{};
  CHECK(stat((root + "/orig").c_str(), &first) == 0);
  CHECK(stat((root + "/hard").c_str(), &second) == 0);
  CHECK(first.st_ino == second.st_ino);
  // Absolute and missing targets fail closed.
  for (const auto *target : {"/etc/hostname", "missing"}) {
    Sandbox nested;
    const auto other = nested.root + "/root";
    CHECK(mkdir(other.c_str(), 0755) == 0);
    TarBuilder evil;
    evil.add("hard", '1', target, 0644, "");
    evil.end();
    TarExtractor other_tar;
    CHECK(other_tar.open(other, {}).ok());
    CHECK(!other_tar.write(evil.bytes).ok());
  }
}

void tar_whiteouts() {
  Sandbox sandbox;
  const auto root = sandbox.root + "/root";
  CHECK(mkdir(root.c_str(), 0755) == 0);
  TarExtractor tar;
  CHECK(tar.open(root, {}).ok());
  TarBuilder first;
  first.file("etc/hostname", "gone\n");
  first.file("etc/keep", "stays\n");
  first.file("gone-dir/inner", "x\n");
  first.end();
  CHECK(tar.write(first.bytes).ok());
  CHECK(tar.finish().ok());
  TarBuilder second;
  second.add("etc/.wh.hostname", '0', "", 0644, "");
  second.dir("etc"); // Merged, not replaced.
  second.add("gone-dir/.wh..wh..opq", '0', "", 0644, "");
  second.end();
  CHECK(tar.write(second.bytes).ok());
  CHECK(tar.finish().ok());
  CHECK(!std::filesystem::exists(root + "/etc/hostname"));
  CHECK(sandbox.read(root + "/etc/keep") == "stays\n");
  CHECK(!std::filesystem::exists(root + "/gone-dir/inner"));
  CHECK(std::filesystem::is_directory(root + "/gone-dir"));
  CHECK(tar.close().ok());
}

void tar_rejections() {
  // Devices, fifos, checksum damage, truncation and trailing data.
  {
    Sandbox sandbox;
    const auto root = sandbox.root + "/root";
    CHECK(mkdir(root.c_str(), 0755) == 0);
    for (const char type : {'3', '4', '6'}) {
      TarBuilder builder;
      builder.add("node", type, "", 0644, "");
      builder.end();
      TarExtractor tar;
      CHECK(tar.open(root, {}).ok());
      CHECK(!tar.write(builder.bytes).ok());
    }
  }
  {
    Sandbox sandbox;
    const auto root = sandbox.root + "/root";
    CHECK(mkdir(root.c_str(), 0755) == 0);
    TarBuilder builder;
    builder.file("f", "data");
    builder.end();
    builder.bytes[148] = builder.bytes[148] == '0' ? '7' : '0'; // Damage it.
    TarExtractor tar;
    CHECK(tar.open(root, {}).ok());
    CHECK(!tar.write(builder.bytes).ok());
  }
  {
    Sandbox sandbox;
    const auto root = sandbox.root + "/root";
    CHECK(mkdir(root.c_str(), 0755) == 0);
    TarBuilder builder;
    builder.file("f", "data");
    builder.end();
    TarExtractor tar;
    CHECK(tar.open(root, {}).ok());
    CHECK(!tar.write(builder.bytes.substr(0, 100)).ok() ||
          tar.finish().code == StatusCode::invalid_argument);
  }
  {
    Sandbox sandbox;
    const auto root = sandbox.root + "/root";
    CHECK(mkdir(root.c_str(), 0755) == 0);
    TarBuilder builder;
    builder.file("f", "data");
    builder.end();
    builder.bytes += "trailing";
    TarExtractor tar;
    CHECK(tar.open(root, {}).ok());
    CHECK(!tar.write(builder.bytes).ok());
  }
  // setuid/setgid bits are stripped; bounds are enforced.
  {
    Sandbox sandbox;
    const auto root = sandbox.root + "/root";
    CHECK(mkdir(root.c_str(), 0755) == 0);
    TarBuilder builder;
    builder.file("suid", "x", 04755);
    builder.end();
    TarExtractor tar;
    CHECK(tar.open(root, {}).ok());
    CHECK(tar.write(builder.bytes).ok());
    CHECK(tar.finish().ok());
    struct stat info{};
    CHECK(stat((root + "/suid").c_str(), &info) == 0);
    CHECK((info.st_mode & 07000) == 0);
  }
  {
    Sandbox sandbox;
    const auto root = sandbox.root + "/root";
    CHECK(mkdir(root.c_str(), 0755) == 0);
    TarBuilder builder;
    builder.file("big", std::string(100, 'b'));
    builder.end();
    TarExtractor tar;
    TarBounds bounds;
    bounds.max_bytes = 10;
    CHECK(tar.open(root, bounds).ok());
    CHECK(tar.write(builder.bytes).code == StatusCode::resource_exhausted);
  }
  {
    Sandbox sandbox;
    const auto root = sandbox.root + "/root";
    CHECK(mkdir(root.c_str(), 0755) == 0);
    TarBuilder builder;
    builder.file("a", "x");
    builder.file("b", "y");
    builder.end();
    TarExtractor tar;
    TarBounds bounds;
    bounds.max_files = 1;
    CHECK(tar.open(root, bounds).ok());
    CHECK(tar.write(builder.bytes).code == StatusCode::resource_exhausted);
  }
}

void tar_longname() {
  Sandbox sandbox;
  const auto root = sandbox.root + "/root";
  CHECK(mkdir(root.c_str(), 0755) == 0);
  const std::string name(200, 'n');
  TarBuilder builder;
  builder.add("./" + name, 'L', "", 0, "./" + name + std::string("\0", 1));
  builder.file(name, "long\n");
  builder.end();
  TarExtractor tar;
  CHECK(tar.open(root, {}).ok());
  CHECK(tar.write(builder.bytes).ok());
  CHECK(tar.finish().ok());
  CHECK(sandbox.read(root + "/" + name) == "long\n");
}

struct LayoutFile {
  std::string name;
  std::string bytes;
};

void write_layout(const std::string &dir,
                  const std::vector<LayoutFile> &layers,
                  const std::vector<std::string> &media_types,
                  const std::string &arch = "amd64",
                  const std::vector<std::string> &diff_ids_in = {}) {
  CHECK(std::filesystem::create_directories(dir + "/blobs"));
  std::ofstream(dir + "/oci-layout") << "{\"imageLayoutVersion\":\"1.0.0\"}";
  std::vector<std::string> diff_ids;
  std::string manifest_layers;
  for (std::size_t i = 0; i < layers.size(); ++i) {
    const auto hex = Sha256::hexdigest(layers[i].bytes);
    std::ofstream(dir + "/blobs/" + hex, std::ios::binary)
        << layers[i].bytes;
    const auto diff =
        (i < diff_ids_in.size() && !diff_ids_in[i].empty()) ? diff_ids_in[i]
                                                            : hex;
    diff_ids.push_back("\"sha256:" + diff + "\"");
    if (i > 0) {
      manifest_layers += ",";
    }
    manifest_layers += "{\"mediaType\":\"" + media_types[i] +
                       "\",\"digest\":\"sha256:" + hex + "\",\"size\":" +
                       std::to_string(layers[i].bytes.size()) + "}";
  }
  std::string diff_list;
  for (std::size_t i = 0; i < diff_ids.size(); ++i) {
    if (i > 0) {
      diff_list += ",";
    }
    diff_list += diff_ids[i];
  }
  const std::string config =
      "{\"architecture\":\"" + arch +
      "\",\"os\":\"linux\",\"rootfs\":{\"type\":\"layers\",\"diff_ids\":[" +
      diff_list + "]}}";
  const auto config_hex = Sha256::hexdigest(config);
  std::ofstream(dir + "/blobs/" + config_hex, std::ios::binary) << config;
  const std::string manifest =
      "{\"schemaVersion\":2,\"mediaType\":\"" +
      std::string(kOciManifestMediaType) +
      "\",\"config\":{\"mediaType\":\"" +
      std::string(kOciConfigMediaType) + "\",\"digest\":\"sha256:" +
      config_hex + "\",\"size\":" + std::to_string(config.size()) +
      "},\"layers\":[" + manifest_layers + "]}";
  const auto manifest_hex = Sha256::hexdigest(manifest);
  std::ofstream(dir + "/blobs/" + manifest_hex, std::ios::binary) << manifest;
  std::ofstream(dir + "/index.json") << "{\"schemaVersion\":2,\"mediaType\":\"" +
      std::string(kOciIndexMediaType) + "\",\"manifests\":[{\"mediaType\":\"" +
      std::string(kOciManifestMediaType) + "\",\"digest\":\"sha256:" +
      manifest_hex + "\",\"size\":" + std::to_string(manifest.size()) +
      ",\"platform\":{\"architecture\":\"" + arch +
      "\",\"os\":\"linux\"}}]}";
}

void image_inspect_unpack() {
  Sandbox sandbox;
  TarBuilder first, second;
  first.dir("etc");
  first.file("etc/hostname", "layer1\n");
  first.file("app/run", "v1\n");
  first.end();
  second.file("app/run", "v2\n");
  second.file("new.txt", "added\n");
  second.add("etc/.wh.hostname", '0', "", 0644, "");
  second.end();
  const std::string gzip_layer(kGzipLayer.begin(), kGzipLayer.end());
  write_layout(sandbox.root + "/layout",
               {{"l1.tar", first.bytes},
                {"l2.tar", second.bytes},
                {"l3.tgz", gzip_layer}},
               {kOciLayerTarMediaType, kOciLayerTarMediaType,
                kOciLayerGzipMediaType},
               "amd64",
               {"", "",
                "51155a88b518b6e7a24223ceda7f0ef81acb90aaa71c18f00ac6cdff7abff49b"});
  ImageLoader loader;
  ImageSummary summary;
  ImagePlatform select;
  select.os = "linux";
  select.architecture = "amd64";
  CHECK(loader.inspect(sandbox.root + "/layout", select, "", summary).ok());
  CHECK(summary.layers.size() == 3);
  CHECK(summary.platform.architecture == "amd64");
  CHECK(summary.config_size > 0);
  // The frozen gzip layer has known identities on both sides.
  CHECK(summary.layers[2].diff_id ==
        "sha256:51155a88b518b6e7a24223ceda7f0ef81acb90aaa71c18f00ac6cdff7abff49b");
  CHECK(summary.layers[2].digest ==
        "sha256:a1673b6b44d5c8d727764d462942194e0fcbad7352c8b744328df867d03dbbe3");
  UnpackOptions options;
  options.layout_directory = sandbox.root + "/layout";
  options.rootfs_directory = sandbox.root + "/rootfs";
  options.platform = select;
  options.expected_digest = summary.manifest_digest;
  UnpackReport report;
  CHECK(loader.unpack(options, report).ok());
  CHECK(report.layers == 3);
  CHECK(report.manifest_digest == summary.manifest_digest);
  CHECK(sandbox.read(sandbox.root + "/rootfs/app/run") == "v2\n");
  CHECK(sandbox.read(sandbox.root + "/rootfs/new.txt") == "added\n");
  CHECK(!std::filesystem::exists(sandbox.root + "/rootfs/etc/hostname"));
  CHECK(sandbox.read(sandbox.root + "/rootfs/g.txt") == "gzipped layer\n");
  CHECK(sandbox.read(sandbox.root + "/rootfs/sub/dir/f.txt") == "nested\n");
  CHECK(report.bytes > 0);
  // Rootfs directories are never reused.
  CHECK(loader.unpack(options, report).code == StatusCode::conflict);
}

void image_rejections() {
  // Tampered blob, wrong diff_id, platform mismatch, bad pin, docker type.
  {
    Sandbox sandbox;
    TarBuilder layer;
    layer.file("f", "x");
    layer.end();
    write_layout(sandbox.root + "/layout", {{"l.tar", layer.bytes}},
                 {kOciLayerTarMediaType});
    const auto hex = Sha256::hexdigest(layer.bytes);
    {
      std::fstream blob(sandbox.root + "/layout/blobs/" + hex,
                        std::ios::binary | std::ios::in | std::ios::out);
      CHECK(static_cast<bool>(blob));
      blob.seekp(600);
      blob.put('X');
    } // Flush the tampered byte before unpacking reads it back.
    ImageLoader loader;
    ImageSummary summary;
    ImagePlatform select{"linux", "amd64"};
    CHECK(loader.inspect(sandbox.root + "/layout", select, "", summary).ok());
    UnpackOptions options;
    options.layout_directory = sandbox.root + "/layout";
    options.rootfs_directory = sandbox.root + "/rootfs";
    options.platform = select;
    UnpackReport report;
    CHECK(!loader.unpack(options, report).ok());
  }
  {
    Sandbox sandbox;
    TarBuilder layer;
    layer.file("f", "x");
    layer.end();
    write_layout(sandbox.root + "/layout", {{"l.tar", layer.bytes}},
                 {kOciLayerTarMediaType}, "amd64",
                 {std::string(64, '0')});
    ImageLoader loader;
    ImageSummary summary;
    ImagePlatform select{"linux", "amd64"};
    CHECK(loader.inspect(sandbox.root + "/layout", select, "", summary).ok());
    UnpackOptions options;
    options.layout_directory = sandbox.root + "/layout";
    options.rootfs_directory = sandbox.root + "/rootfs";
    options.platform = select;
    UnpackReport report;
    CHECK(loader.unpack(options, report).code == StatusCode::invalid_argument);
  }
  {
    Sandbox sandbox;
    TarBuilder layer;
    layer.file("f", "x");
    layer.end();
    write_layout(sandbox.root + "/layout", {{"l.tar", layer.bytes}},
                 {kOciLayerTarMediaType});
    ImageLoader loader;
    ImageSummary summary;
    ImagePlatform select{"linux", "arm64"};
    CHECK(loader.inspect(sandbox.root + "/layout", select, "", summary).code ==
          StatusCode::not_found);
    ImagePlatform pinned{"linux", "amd64"};
    CHECK(loader.inspect(sandbox.root + "/layout", pinned,
                         "sha256:" + std::string(64, '0'), summary)
              .code == StatusCode::invalid_argument);
  }
  {
    Sandbox sandbox;
    TarBuilder layer;
    layer.file("f", "x");
    layer.end();
    write_layout(sandbox.root + "/layout", {{"l.tar", layer.bytes}},
                 {"application/vnd.docker.image.rootfs.diff.tar"});
    ImageLoader loader;
    ImageSummary summary;
    ImagePlatform select{"linux", "amd64"};
    CHECK(loader.inspect(sandbox.root + "/layout", select, "", summary).code ==
          StatusCode::not_implemented);
  }
  {
    Sandbox sandbox;
    ImageLoader loader;
    ImageSummary summary;
    ImagePlatform select{"linux", "amd64"};
    CHECK(!loader.inspect(sandbox.root + "/missing", select, "", summary)
               .ok());
  }
}

} // namespace

int main() {
  return test::run([] {
    gzip_vectors();
    tar_basics();
    tar_traversal_rejected();
    tar_hardlinks();
    tar_whiteouts();
    tar_rejections();
    tar_longname();
    image_inspect_unpack();
    image_rejections();
  });
}

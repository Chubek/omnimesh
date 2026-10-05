#include "omnimesh/sha256.hpp"

namespace omnimesh {
namespace {

inline std::uint32_t rotate(std::uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32 - bits));
}

// First 32 bits of the fractional parts of the cube roots of the first 64
// primes (FIPS 180-4, section 4.2.2).
constexpr std::uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

} // namespace

Sha256::Sha256()
    : // First 32 bits of the fractional parts of the square roots of the
      // first 8 primes (FIPS 180-4, section 5.3.3).
      state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19},
      block_{} {}

void Sha256::update(const void *data, std::size_t size) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  while (size > 0) {
    const std::size_t room = block_.size() - buffered_;
    const std::size_t take = size < room ? size : room;
    for (std::size_t i = 0; i < take; ++i) {
      block_[buffered_ + i] = bytes[i];
    }
    buffered_ += take;
    bytes += take;
    size -= take;
    total_ += take;
    if (buffered_ == block_.size()) {
      compress();
      buffered_ = 0;
    }
  }
}

void Sha256::update(std::string_view data) {
  update(data.data(), data.size());
}

void Sha256::compress() {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t i = 0; i < 16; ++i) {
    schedule[i] = (static_cast<std::uint32_t>(block_[4 * i]) << 24) |
                  (static_cast<std::uint32_t>(block_[4 * i + 1]) << 16) |
                  (static_cast<std::uint32_t>(block_[4 * i + 2]) << 8) |
                  static_cast<std::uint32_t>(block_[4 * i + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        rotate(schedule[i - 15], 7) ^ rotate(schedule[i - 15], 18) ^
        (schedule[i - 15] >> 3);
    const std::uint32_t s1 =
        rotate(schedule[i - 2], 17) ^ rotate(schedule[i - 2], 19) ^
        (schedule[i - 2] >> 10);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }
  std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + s1 + ch + kRound[i] + schedule[i];
    const std::uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

std::array<std::uint8_t, 32> Sha256::finish() const {
  Sha256 padded = *this;
  const std::uint64_t bits = padded.total_ * 8;
  unsigned char pad = 0x80;
  padded.update(&pad, 1);
  pad = 0x00;
  while (padded.buffered_ != 56) {
    padded.update(&pad, 1);
  }
  unsigned char length[8]{};
  for (unsigned i = 0; i < 8; ++i) {
    length[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
  }
  padded.update(length, sizeof(length));
  std::array<std::uint8_t, 32> digest{};
  for (std::size_t i = 0; i < 8; ++i) {
    digest[4 * i] = static_cast<std::uint8_t>(padded.state_[i] >> 24);
    digest[4 * i + 1] = static_cast<std::uint8_t>(padded.state_[i] >> 16);
    digest[4 * i + 2] = static_cast<std::uint8_t>(padded.state_[i] >> 8);
    digest[4 * i + 3] = static_cast<std::uint8_t>(padded.state_[i]);
  }
  return digest;
}

std::string Sha256::hex(const std::array<std::uint8_t, 32> &digest) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  text.reserve(64);
  for (const auto byte : digest) {
    text.push_back(kDigits[byte >> 4]);
    text.push_back(kDigits[byte & 0x0f]);
  }
  return text;
}

std::string Sha256::hexdigest(std::string_view data) {
  Sha256 hash;
  hash.update(data);
  return hex(hash.finish());
}

} // namespace omnimesh

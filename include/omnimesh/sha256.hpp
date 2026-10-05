#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace omnimesh {

// Incremental SHA-256 (FIPS 180-4) over bounded inputs. Fixed-size state with
// no dynamic allocation; callers bound the total bytes hashed.
class Sha256 {
public:
  Sha256();
  void update(const void *data, std::size_t size);
  void update(std::string_view data);
  // Finalize on a copy: calling finish() never disturbs further updates, so
  // intermediate digests can be taken safely.
  std::array<std::uint8_t, 32> finish() const;

  static std::string hexdigest(std::string_view data);
  static std::string hex(const std::array<std::uint8_t, 32> &digest);

private:
  void compress();
  std::array<std::uint32_t, 8> state_;
  std::array<unsigned char, 64> block_;
  std::size_t buffered_{0};
  std::uint64_t total_{0};
};

} // namespace omnimesh

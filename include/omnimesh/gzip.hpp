#pragma once

#include "omnimesh/status.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace omnimesh {

// Streaming gzip (RFC 1952) and DEFLATE (RFC 1951) decoder over bounded
// inputs. The source returns a positive stored-byte count, zero on
// end-of-input, or a negative errno. Only fixed and dynamic Huffman blocks,
// stored blocks, header flags and concatenated members are supported; the
// output bound is enforced incrementally and trailers (CRC32, length) are
// verified before any further output is served.
class GzipReader {
public:
  using Source = std::function<ssize_t(unsigned char *, std::size_t)>;
  GzipReader(Source source, std::uint64_t max_output);
  // Stores up to `capacity` verified-output bytes and reports them in `got`.
  // A zero `got` with Ok means the verified end of stream. `capacity` must
  // be positive. Reads never retain more than a bounded window plus one
  // source chunk.
  Status read(unsigned char *out, std::size_t capacity, std::size_t &got);

private:
  Status fill();
  Status get_byte(unsigned &byte);
  Status get_block(std::uint32_t &value, unsigned bits);
  Status get_bytes(unsigned char *target, std::size_t count);
  Status skip_bytes(std::uint64_t count);
  Status parse_header();
  Status parse_trailer();
  Status decode_tables();
  Status decode_symbol(const unsigned *lengths, const unsigned *symbols,
                       unsigned count, unsigned &symbol);
  Status start_block();
  Status copy_match();
  Status decode_symbols();
  Status emit(unsigned char byte);

  Source source_;
  std::uint64_t max_output_;
  std::uint64_t output_{0};
  std::uint32_t crc_{0xffffffffU};
  unsigned members_{0};
  bool started_{false};
  bool finished_{false};

  // Bit buffer, LSB first.
  std::uint64_t bits_{0};
  unsigned available_{0};
  unsigned char chunk_[8192];
  std::size_t chunk_used_{0};
  std::size_t chunk_next_{0};
  bool input_ended_{false};

  // DEFLATE tables for the current block.
  unsigned lit_lengths_[288]{};
  unsigned lit_symbols_[288]{};
  unsigned dist_lengths_[32]{};
  unsigned dist_symbols_[32]{};
  bool fixed_tables_{false};
  bool have_dist_{false};

  // Resume state across read() calls.
  unsigned char *out_{nullptr};
  std::size_t out_left_{0};
  bool need_trailer_{false};
  bool in_block_{false};
  bool block_last_{false};
  unsigned block_type_{0};
  unsigned stored_left_{0};
  unsigned match_left_{0};
  unsigned match_dist_{0};
  unsigned char window_[32768]{};
  std::size_t window_next_{0};
};

} // namespace omnimesh

#pragma once

#include "lanternstage/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lanternstage {

class WireReader {
 public:
  WireReader(const std::uint8_t* data, std::size_t size);
  explicit WireReader(ByteSpan bytes);

  std::size_t remaining() const;
  std::size_t position() const;
  bool empty() const;

  bool read_u8(std::uint8_t& value);
  bool read_u16(std::uint16_t& value);
  bool read_u32(std::uint32_t& value);
  bool read_u64(std::uint64_t& value);
  bool read_i32(std::int32_t& value);
  bool read_varuint(std::uint64_t& value);
  bool read_bytes(std::size_t count, ByteSpan& value);
  bool read_string(std::size_t count, std::string& value);
  bool skip(std::size_t count);
  bool seek(std::size_t absolute_offset);

  ByteSpan tail() const;

 private:
  const std::uint8_t* data_{nullptr};
  std::size_t size_{0};
  std::size_t offset_{0};
};

class WireWriter {
 public:
  void write_u8(std::uint8_t value);
  void write_u16(std::uint16_t value);
  void write_u32(std::uint32_t value);
  void write_u64(std::uint64_t value);
  void write_i32(std::int32_t value);
  void write_varuint(std::uint64_t value);
  void write_bytes(ByteSpan bytes);
  void write_string(std::string_view value);

  const std::vector<std::uint8_t>& bytes() const;
  std::vector<std::uint8_t> take();
  void clear();

 private:
  std::vector<std::uint8_t> bytes_;
};

std::uint32_t fnv1a32(ByteSpan bytes, std::uint32_t seed = 2166136261u);
std::uint64_t fnv1a64(ByteSpan bytes, std::uint64_t seed = 1469598103934665603ull);
std::uint32_t mix32(std::uint32_t x);
std::uint64_t mix64(std::uint64_t x);

}  // namespace lanternstage

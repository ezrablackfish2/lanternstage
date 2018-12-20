#include "lanternstage/wire.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace lanternstage {

WireReader::WireReader(const std::uint8_t* data, std::size_t size)
    : data_(data), size_(size) {}

WireReader::WireReader(ByteSpan bytes) : data_(bytes.data), size_(bytes.size) {}

std::size_t WireReader::remaining() const { return size_ - offset_; }
std::size_t WireReader::position() const { return offset_; }
bool WireReader::empty() const { return offset_ == size_; }

bool WireReader::read_u8(std::uint8_t& value) {
  if (remaining() < 1) return false;
  value = data_[offset_++];
  return true;
}

bool WireReader::read_u16(std::uint16_t& value) {
  if (remaining() < 2) return false;
  value = static_cast<std::uint16_t>(data_[offset_]) |
          (static_cast<std::uint16_t>(data_[offset_ + 1]) << 8u);
  offset_ += 2;
  return true;
}

bool WireReader::read_u32(std::uint32_t& value) {
  if (remaining() < 4) return false;
  value = static_cast<std::uint32_t>(data_[offset_]) |
          (static_cast<std::uint32_t>(data_[offset_ + 1]) << 8u) |
          (static_cast<std::uint32_t>(data_[offset_ + 2]) << 16u) |
          (static_cast<std::uint32_t>(data_[offset_ + 3]) << 24u);
  offset_ += 4;
  return true;
}

bool WireReader::read_u64(std::uint64_t& value) {
  std::uint32_t low = 0;
  std::uint32_t high = 0;
  if (!read_u32(low) || !read_u32(high)) return false;
  value = static_cast<std::uint64_t>(low) |
          (static_cast<std::uint64_t>(high) << 32u);
  return true;
}

bool WireReader::read_i32(std::int32_t& value) {
  std::uint32_t raw = 0;
  if (!read_u32(raw)) return false;
  std::memcpy(&value, &raw, sizeof(value));
  return true;
}

bool WireReader::read_varuint(std::uint64_t& value) {
  value = 0;
  unsigned shift = 0;
  for (unsigned i = 0; i < 10; ++i) {
    std::uint8_t byte = 0;
    if (!read_u8(byte)) return false;
    if (shift == 63u && (byte & 0xfeu) != 0) return false;
    value |= static_cast<std::uint64_t>(byte & 0x7fu) << shift;
    if ((byte & 0x80u) == 0) return true;
    shift += 7u;
  }
  return false;
}

bool WireReader::read_bytes(std::size_t count, ByteSpan& value) {
  if (count > remaining()) return false;
  value = {data_ + offset_, count};
  offset_ += count;
  return true;
}

bool WireReader::read_string(std::size_t count, std::string& value) {
  ByteSpan bytes;
  if (!read_bytes(count, bytes)) return false;
  value.assign(reinterpret_cast<const char*>(bytes.data), bytes.size);
  return true;
}

bool WireReader::skip(std::size_t count) {
  if (count > remaining()) return false;
  offset_ += count;
  return true;
}

bool WireReader::seek(std::size_t absolute_offset) {
  if (absolute_offset > size_) return false;
  offset_ = absolute_offset;
  return true;
}

ByteSpan WireReader::tail() const { return {data_ + offset_, remaining()}; }

void WireWriter::write_u8(std::uint8_t value) { bytes_.push_back(value); }

void WireWriter::write_u16(std::uint16_t value) {
  bytes_.push_back(static_cast<std::uint8_t>(value));
  bytes_.push_back(static_cast<std::uint8_t>(value >> 8u));
}

void WireWriter::write_u32(std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void WireWriter::write_u64(std::uint64_t value) {
  write_u32(static_cast<std::uint32_t>(value));
  write_u32(static_cast<std::uint32_t>(value >> 32u));
}

void WireWriter::write_i32(std::int32_t value) {
  std::uint32_t raw = 0;
  std::memcpy(&raw, &value, sizeof(raw));
  write_u32(raw);
}

void WireWriter::write_varuint(std::uint64_t value) {
  while (value >= 0x80u) {
    write_u8(static_cast<std::uint8_t>(value) | 0x80u);
    value >>= 7u;
  }
  write_u8(static_cast<std::uint8_t>(value));
}

void WireWriter::write_bytes(ByteSpan bytes) {
  if (bytes.size == 0) return;
  bytes_.insert(bytes_.end(), bytes.data, bytes.data + bytes.size);
}

void WireWriter::write_string(std::string_view value) {
  write_bytes({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

const std::vector<std::uint8_t>& WireWriter::bytes() const { return bytes_; }
std::vector<std::uint8_t> WireWriter::take() { return std::move(bytes_); }
void WireWriter::clear() { bytes_.clear(); }

std::uint32_t fnv1a32(ByteSpan bytes, std::uint32_t seed) {
  std::uint32_t hash = seed;
  for (std::size_t i = 0; i < bytes.size; ++i) {
    hash ^= bytes.data[i];
    hash *= 16777619u;
  }
  return hash;
}

std::uint64_t fnv1a64(ByteSpan bytes, std::uint64_t seed) {
  std::uint64_t hash = seed;
  for (std::size_t i = 0; i < bytes.size; ++i) {
    hash ^= bytes.data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

std::uint32_t mix32(std::uint32_t x) {
  x ^= x >> 16u;
  x *= 0x7feb352du;
  x ^= x >> 15u;
  x *= 0x846ca68bu;
  x ^= x >> 16u;
  return x;
}

std::uint64_t mix64(std::uint64_t x) {
  x ^= x >> 30u;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27u;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31u;
  return x;
}

}  // namespace lanternstage

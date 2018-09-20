#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lanternstage {

using AssetId = std::uint32_t;
using PlaylistId = std::uint32_t;
using DeviceId = std::uint32_t;
using TenantId = std::uint32_t;
using Revision = std::uint32_t;
using Epoch = std::uint32_t;
using Tick = std::uint64_t;

struct Status {
  enum class Code : std::uint8_t {
    ok = 0,
    malformed,
    unsupported,
    not_found,
    conflict,
    exhausted,
    rejected,
    internal_error,
  };

  Code code{Code::ok};
  std::string message;

  static Status success() { return {}; }
  static Status failure(Code c, std::string text) {
    Status out;
    out.code = c;
    out.message = std::move(text);
    return out;
  }
  explicit operator bool() const { return code == Code::ok; }
};

template <typename T>
struct Result {
  Status status;
  T value{};

  explicit operator bool() const { return static_cast<bool>(status); }
};

struct ByteSpan {
  const std::uint8_t* data{nullptr};
  std::size_t size{0};

  ByteSpan subspan(std::size_t offset, std::size_t length) const {
    if (offset > size || length > size - offset) {
      return {};
    }
    return {data + offset, length};
  }
};

struct MutableByteSpan {
  std::uint8_t* data{nullptr};
  std::size_t size{0};
};

struct Rect {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};

  bool empty() const { return width <= 0 || height <= 0; }
};

struct Rational {
  std::int64_t numerator{0};
  std::int64_t denominator{1};
};

struct SemanticVersion {
  std::uint16_t major{0};
  std::uint16_t minor{0};
  std::uint16_t patch{0};
};

}  // namespace lanternstage

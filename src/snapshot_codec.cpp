#include "lanternstage/snapshot_codec.h"

#include "lanternstage/text.h"
#include "lanternstage/wire.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace lanternstage {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{{'L', 'S', 'S', 'N'}};
constexpr std::size_t kHeaderSize = 48;
constexpr std::size_t kChunkHeaderSize = 36;

std::uint64_t chunk_digest(const SnapshotChunk& chunk) {
  std::uint64_t hash = mix64(chunk.type ^ chunk.flags ^ chunk.logical_offset);
  hash ^= fnv1a64({chunk.bytes.data(), chunk.bytes.size()});
  return mix64(hash ^ chunk.bytes.size());
}

bool add_size(std::size_t& total, std::size_t value, std::size_t maximum) {
  if (value > maximum - std::min(total, maximum)) return false;
  total += value;
  return total <= maximum;
}

}  // namespace

Result<EngineSnapshot> decode_snapshot(ByteSpan bytes, const SnapshotLimits& limits) {
  Result<EngineSnapshot> result;
  if (bytes.size < kHeaderSize || !std::equal(kMagic.begin(), kMagic.end(), bytes.data)) {
    result.status = Status::failure(Status::Code::malformed, "snapshot header is missing");
    return result;
  }
  WireReader reader(bytes);
  ByteSpan magic;
  std::uint16_t chunk_count = 0;
  std::uint16_t metadata_count = 0;
  std::uint32_t declared_size = 0;
  std::uint64_t declared_digest = 0;
  std::uint32_t reserved = 0;
  if (!reader.read_bytes(4, magic) || !reader.read_u16(result.value.version.major) ||
      !reader.read_u16(result.value.version.minor) || !reader.read_u16(result.value.version.patch) ||
      !reader.read_u16(chunk_count) || !reader.read_u16(metadata_count) || !reader.skip(2) ||
      !reader.read_u32(result.value.stream_id) || !reader.read_u32(result.value.revision) ||
      !reader.read_u64(result.value.clock) || !reader.read_u32(declared_size) ||
      !reader.read_u64(declared_digest) || !reader.read_u32(reserved)) {
    result.status = Status::failure(Status::Code::malformed, "snapshot header is truncated");
    return result;
  }
  if (declared_size != bytes.size || chunk_count > limits.maximum_chunks ||
      metadata_count > limits.maximum_metadata_entries || reserved != 0) {
    result.status = Status::failure(Status::Code::rejected, "snapshot header exceeds limits");
    return result;
  }
  std::size_t metadata_bytes = 0;
  for (std::uint16_t i = 0; i < metadata_count; ++i) {
    std::uint16_t key_size = 0;
    std::uint16_t value_size = 0;
    if (!reader.read_u16(key_size) || !reader.read_u16(value_size) ||
        !add_size(metadata_bytes, key_size + value_size, limits.maximum_metadata_bytes)) {
      result.status = Status::failure(Status::Code::malformed, "snapshot metadata header is invalid");
      return result;
    }
    std::string key;
    std::string value;
    if (!reader.read_string(key_size, key) || !reader.read_string(value_size, value) ||
        key.empty() || !validate_utf8(key) || !validate_utf8(value) ||
        !result.value.metadata.emplace(std::move(key), std::move(value)).second) {
      result.status = Status::failure(Status::Code::conflict, "snapshot metadata is invalid or duplicated");
      return result;
    }
  }

  struct PendingChunk {
    SnapshotChunk chunk;
    std::uint32_t stored_size{0};
    std::uint32_t raw_size{0};
    std::uint64_t digest{0};
  };
  std::vector<PendingChunk> pending;
  pending.reserve(chunk_count);
  std::size_t total_raw_size = 0;
  for (std::uint16_t i = 0; i < chunk_count; ++i) {
    PendingChunk item;
    std::uint32_t reserved_chunk = 0;
    if (!reader.read_u32(item.chunk.type) || !reader.read_u32(item.chunk.flags) ||
        !reader.read_u64(item.chunk.logical_offset) || !reader.read_u32(item.stored_size) ||
        !reader.read_u32(item.raw_size) || !reader.read_u64(item.digest) ||
        !reader.read_u32(reserved_chunk)) {
      result.status = Status::failure(Status::Code::malformed, "snapshot chunk table is truncated");
      return result;
    }
    if (item.chunk.type == 0 || item.raw_size > limits.maximum_chunk_size ||
        item.stored_size != item.raw_size || reserved_chunk != 0 ||
        !add_size(total_raw_size, item.raw_size, limits.maximum_total_size)) {
      result.status = Status::failure(Status::Code::rejected, "snapshot chunk table exceeds limits");
      return result;
    }
    pending.push_back(std::move(item));
  }
  for (PendingChunk& item : pending) {
    ByteSpan payload;
    if (!reader.read_bytes(item.stored_size, payload)) {
      result.status = Status::failure(Status::Code::malformed, "snapshot chunk payload is truncated");
      return result;
    }
    item.chunk.bytes.assign(payload.data, payload.data + payload.size);
    if (chunk_digest(item.chunk) != item.digest) {
      result.status = Status::failure(Status::Code::malformed, "snapshot chunk digest mismatch");
      return result;
    }
    result.value.chunks.push_back(std::move(item.chunk));
  }
  if (!reader.empty() || snapshot_digest(result.value) != declared_digest) {
    result.status = Status::failure(Status::Code::malformed, "snapshot digest or trailing bytes mismatch");
    return result;
  }
  result.status = validate_snapshot(result.value, limits);
  return result;
}

std::vector<std::uint8_t> encode_snapshot(const EngineSnapshot& snapshot) {
  WireWriter metadata;
  std::vector<std::pair<std::string, std::string>> ordered(snapshot.metadata.begin(), snapshot.metadata.end());
  std::sort(ordered.begin(), ordered.end());
  for (const auto& [key, value] : ordered) {
    metadata.write_u16(static_cast<std::uint16_t>(key.size()));
    metadata.write_u16(static_cast<std::uint16_t>(value.size()));
    metadata.write_string(key);
    metadata.write_string(value);
  }
  WireWriter table;
  WireWriter payloads;
  for (const SnapshotChunk& chunk : snapshot.chunks) {
    table.write_u32(chunk.type);
    table.write_u32(chunk.flags);
    table.write_u64(chunk.logical_offset);
    table.write_u32(static_cast<std::uint32_t>(chunk.bytes.size()));
    table.write_u32(static_cast<std::uint32_t>(chunk.bytes.size()));
    table.write_u64(chunk_digest(chunk));
    table.write_u32(0);
    payloads.write_bytes({chunk.bytes.data(), chunk.bytes.size()});
  }
  const std::size_t total_size = kHeaderSize + metadata.bytes().size() +
                                 table.bytes().size() + payloads.bytes().size();
  WireWriter out;
  out.write_bytes({kMagic.data(), kMagic.size()});
  out.write_u16(snapshot.version.major);
  out.write_u16(snapshot.version.minor);
  out.write_u16(snapshot.version.patch);
  out.write_u16(static_cast<std::uint16_t>(snapshot.chunks.size()));
  out.write_u16(static_cast<std::uint16_t>(ordered.size()));
  out.write_u16(0);
  out.write_u32(snapshot.stream_id);
  out.write_u32(snapshot.revision);
  out.write_u64(snapshot.clock);
  out.write_u32(static_cast<std::uint32_t>(total_size));
  out.write_u64(snapshot_digest(snapshot));
  out.write_u32(0);
  out.write_bytes({metadata.bytes().data(), metadata.bytes().size()});
  out.write_bytes({table.bytes().data(), table.bytes().size()});
  out.write_bytes({payloads.bytes().data(), payloads.bytes().size()});
  return out.take();
}

Status validate_snapshot(const EngineSnapshot& snapshot, const SnapshotLimits& limits) {
  if (snapshot.version.major != 1)
    return Status::failure(Status::Code::unsupported, "snapshot major version is unsupported");
  if (snapshot.chunks.size() > limits.maximum_chunks ||
      snapshot.metadata.size() > limits.maximum_metadata_entries)
    return Status::failure(Status::Code::exhausted, "snapshot contains too many entries");
  std::size_t total = 0;
  std::set<std::pair<std::uint32_t, std::uint64_t>> starts;
  for (const SnapshotChunk& chunk : snapshot.chunks) {
    if (chunk.type == 0 || chunk.bytes.size() > limits.maximum_chunk_size ||
        !add_size(total, chunk.bytes.size(), limits.maximum_total_size))
      return Status::failure(Status::Code::rejected, "snapshot chunk is invalid");
    if (!starts.emplace(chunk.type, chunk.logical_offset).second)
      return Status::failure(Status::Code::conflict, "snapshot has duplicate chunk offsets");
    if (chunk.logical_offset + chunk.bytes.size() < chunk.logical_offset)
      return Status::failure(Status::Code::exhausted, "snapshot chunk range overflows");
  }
  std::size_t metadata_bytes = 0;
  for (const auto& [key, value] : snapshot.metadata) {
    if (key.empty() || !validate_utf8(key) || !validate_utf8(value) ||
        !add_size(metadata_bytes, key.size() + value.size(), limits.maximum_metadata_bytes))
      return Status::failure(Status::Code::rejected, "snapshot metadata is invalid");
  }
  return Status::success();
}

std::uint64_t snapshot_digest(const EngineSnapshot& snapshot) {
  std::uint64_t hash = mix64(snapshot.stream_id ^ snapshot.revision ^ snapshot.clock);
  hash ^= mix64((static_cast<std::uint64_t>(snapshot.version.major) << 32u) |
                (static_cast<std::uint64_t>(snapshot.version.minor) << 16u) |
                snapshot.version.patch);
  std::vector<std::pair<std::string, std::string>> metadata(snapshot.metadata.begin(), snapshot.metadata.end());
  std::sort(metadata.begin(), metadata.end());
  for (const auto& [key, value] : metadata) {
    hash ^= stable_text_hash(key);
    hash = mix64(hash ^ stable_text_hash(value));
  }
  std::vector<const SnapshotChunk*> chunks;
  for (const SnapshotChunk& chunk : snapshot.chunks) chunks.push_back(&chunk);
  std::sort(chunks.begin(), chunks.end(), [](const SnapshotChunk* a, const SnapshotChunk* b) {
    if (a->type != b->type) return a->type < b->type;
    return a->logical_offset < b->logical_offset;
  });
  for (const SnapshotChunk* chunk : chunks) hash = mix64(hash ^ chunk_digest(*chunk));
  return hash;
}

std::vector<SnapshotChunk> coalesce_snapshot_chunks(
    const std::vector<SnapshotChunk>& chunks, std::size_t maximum_gap) {
  std::vector<SnapshotChunk> ordered = chunks;
  std::sort(ordered.begin(), ordered.end(), [](const SnapshotChunk& a, const SnapshotChunk& b) {
    if (a.type != b.type) return a.type < b.type;
    if (a.flags != b.flags) return a.flags < b.flags;
    return a.logical_offset < b.logical_offset;
  });
  std::vector<SnapshotChunk> result;
  for (SnapshotChunk& chunk : ordered) {
    if (result.empty()) {
      result.push_back(std::move(chunk));
      continue;
    }
    SnapshotChunk& previous = result.back();
    const std::uint64_t previous_end = previous.logical_offset + previous.bytes.size();
    if (previous.type == chunk.type && previous.flags == chunk.flags &&
        chunk.logical_offset >= previous_end && chunk.logical_offset - previous_end <= maximum_gap) {
      previous.bytes.insert(previous.bytes.end(),
                            static_cast<std::size_t>(chunk.logical_offset - previous_end), 0);
      previous.bytes.insert(previous.bytes.end(), chunk.bytes.begin(), chunk.bytes.end());
    } else {
      result.push_back(std::move(chunk));
    }
  }
  return result;
}

std::vector<std::uint8_t> extract_snapshot_range(
    const EngineSnapshot& snapshot, std::uint32_t type,
    std::uint64_t offset, std::size_t length) {
  std::vector<std::uint8_t> result(length, 0);
  if (offset + length < offset) return {};
  const std::uint64_t requested_end = offset + length;
  for (const SnapshotChunk& chunk : snapshot.chunks) {
    if (chunk.type != type) continue;
    const std::uint64_t chunk_end = chunk.logical_offset + chunk.bytes.size();
    const std::uint64_t begin = std::max(offset, chunk.logical_offset);
    const std::uint64_t end = std::min(requested_end, chunk_end);
    if (begin >= end) continue;
    std::copy(chunk.bytes.begin() + static_cast<std::ptrdiff_t>(begin - chunk.logical_offset),
              chunk.bytes.begin() + static_cast<std::ptrdiff_t>(end - chunk.logical_offset),
              result.begin() + static_cast<std::ptrdiff_t>(begin - offset));
  }
  return result;
}

}  // namespace lanternstage

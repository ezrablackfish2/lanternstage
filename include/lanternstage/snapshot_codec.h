#pragma once

#include "lanternstage/types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lanternstage {

struct SnapshotChunk {
  std::uint32_t type{0};
  std::uint32_t flags{0};
  std::uint64_t logical_offset{0};
  std::vector<std::uint8_t> bytes;
};

struct EngineSnapshot {
  SemanticVersion version{1, 0, 0};
  std::uint32_t stream_id{0};
  Revision revision{0};
  Tick clock{0};
  std::vector<SnapshotChunk> chunks;
  std::unordered_map<std::string, std::string> metadata;
};

struct SnapshotLimits {
  std::size_t maximum_chunks{4096};
  std::size_t maximum_chunk_size{16 * 1024 * 1024};
  std::size_t maximum_total_size{128 * 1024 * 1024};
  std::size_t maximum_metadata_entries{1024};
  std::size_t maximum_metadata_bytes{1024 * 1024};
};

Result<EngineSnapshot> decode_snapshot(ByteSpan bytes,
                                       const SnapshotLimits& limits = {});
std::vector<std::uint8_t> encode_snapshot(const EngineSnapshot& snapshot);
Status validate_snapshot(const EngineSnapshot& snapshot,
                         const SnapshotLimits& limits = {});
std::uint64_t snapshot_digest(const EngineSnapshot& snapshot);
std::vector<SnapshotChunk> coalesce_snapshot_chunks(
    const std::vector<SnapshotChunk>& chunks,
    std::size_t maximum_gap = 0);
std::vector<std::uint8_t> extract_snapshot_range(
    const EngineSnapshot& snapshot,
    std::uint32_t type,
    std::uint64_t offset,
    std::size_t length);

}  // namespace lanternstage

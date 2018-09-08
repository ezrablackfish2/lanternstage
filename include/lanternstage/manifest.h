#pragma once

#include "lanternstage/diagnostics.h"
#include "lanternstage/time_math.h"
#include "lanternstage/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lanternstage {

struct ManifestAsset {
  AssetId id{0};
  std::string path;
  std::string media_type;
  std::string locale;
  std::uint64_t byte_size{0};
  std::uint64_t content_hash{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::vector<std::string> tags;
};

struct ManifestPlaylistItem {
  std::uint32_t id{0};
  AssetId asset_id{0};
  std::uint32_t duration_ms{0};
  std::string transition;
  std::optional<std::uint32_t> next_item;
  std::unordered_map<std::string, std::string> metadata;
};

struct ManifestPlaylist {
  PlaylistId id{0};
  std::string name;
  std::uint32_t root_item{0};
  std::vector<ManifestPlaylistItem> items;
  std::optional<RecurrenceRule> recurrence;
  std::optional<CivilDateTime> starts_at;
  std::optional<CivilDateTime> ends_at;
};

struct ManifestDeviceGroup {
  std::uint32_t id{0};
  std::string name;
  std::uint32_t required_capabilities{0};
  std::vector<DeviceId> devices;
  std::vector<PlaylistId> playlists;
};

struct PackageManifest {
  SemanticVersion format_version;
  std::string package_id;
  std::string title;
  std::string default_locale;
  Revision revision{0};
  std::vector<ManifestAsset> assets;
  std::vector<ManifestPlaylist> playlists;
  std::vector<ManifestDeviceGroup> device_groups;
  std::unordered_map<std::string, std::string> properties;
};

Result<PackageManifest> parse_manifest_text(std::string_view text,
                                            DiagnosticSink* diagnostics = nullptr,
                                            std::string source_name = "manifest.lst");
Status validate_manifest(const PackageManifest& manifest,
                         DiagnosticSink* diagnostics = nullptr);
std::string serialize_manifest_text(const PackageManifest& manifest);
std::uint64_t manifest_digest(const PackageManifest& manifest);
std::vector<AssetId> referenced_assets(const PackageManifest& manifest);
std::vector<PlaylistId> playlists_for_device(const PackageManifest& manifest,
                                             DeviceId device,
                                             std::uint32_t capabilities);

}  // namespace lanternstage

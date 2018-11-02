#include "lanternstage/manifest.h"

#include "lanternstage/text.h"
#include "lanternstage/wire.h"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>

namespace lanternstage {
namespace {

struct ParsedLine {
  std::string section;
  std::string key;
  std::string value;
  std::size_t begin{0};
  std::size_t end{0};
};

bool parse_u32(std::string_view text, std::uint32_t& value) {
  const char* begin = text.data();
  const char* end = begin + text.size();
  auto parsed = std::from_chars(begin, end, value);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_u64(std::string_view text, std::uint64_t& value) {
  const char* begin = text.data();
  const char* end = begin + text.size();
  auto parsed = std::from_chars(begin, end, value);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_version(std::string_view text, SemanticVersion& version) {
  std::vector<std::string> parts = split_ascii(text, '.', true);
  if (parts.size() != 3) return false;
  std::uint32_t values[3]{};
  for (std::size_t i = 0; i < 3; ++i) {
    if (!parse_u32(parts[i], values[i]) || values[i] > 65535) return false;
  }
  version.major = static_cast<std::uint16_t>(values[0]);
  version.minor = static_cast<std::uint16_t>(values[1]);
  version.patch = static_cast<std::uint16_t>(values[2]);
  return true;
}

std::vector<ParsedLine> lex_lines(std::string_view text, DiagnosticSink& diagnostics,
                                  const SourceMap& source) {
  std::vector<ParsedLine> lines;
  std::string section = "package";
  std::size_t offset = 0;
  while (offset <= text.size()) {
    const std::size_t newline = text.find('\n', offset);
    const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
    std::string line = trim_ascii(text.substr(offset, end - offset));
    const std::size_t begin = offset;
    offset = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
    if (line.empty() || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = trim_ascii(std::string_view(line).substr(1, line.size() - 2));
      if (section.empty()) diagnostics.error("M001", "section name cannot be empty", source.range(begin, end));
      continue;
    }
    const std::size_t equal = line.find('=');
    if (equal == std::string::npos) {
      diagnostics.error("M002", "manifest line requires key=value", source.range(begin, end));
      continue;
    }
    ParsedLine parsed;
    parsed.section = section;
    parsed.key = trim_ascii(std::string_view(line).substr(0, equal));
    parsed.value = trim_ascii(std::string_view(line).substr(equal + 1));
    parsed.begin = begin;
    parsed.end = end;
    if (parsed.key.empty()) diagnostics.error("M003", "manifest key cannot be empty", source.range(begin, end));
    lines.push_back(std::move(parsed));
  }
  return lines;
}

std::unordered_map<std::string, std::string> collect_section(
    const std::vector<ParsedLine>& lines, std::string_view section,
    DiagnosticSink& diagnostics, const SourceMap& source) {
  std::unordered_map<std::string, std::string> values;
  for (const ParsedLine& line : lines) {
    if (line.section != section) continue;
    if (!values.emplace(line.key, line.value).second) {
      diagnostics.error("M004", "duplicate key '" + line.key + "' in section '" + line.section + "'",
                        source.range(line.begin, line.end));
    }
  }
  return values;
}

std::vector<std::string> section_names(const std::vector<ParsedLine>& lines,
                                       std::string_view prefix) {
  std::vector<std::string> names;
  for (const ParsedLine& line : lines) {
    if (line.section.rfind(prefix, 0) == 0 &&
        std::find(names.begin(), names.end(), line.section) == names.end()) {
      names.push_back(line.section);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string require(const std::unordered_map<std::string, std::string>& values,
                    std::string_view key, std::string_view section,
                    DiagnosticSink& diagnostics) {
  auto it = values.find(std::string(key));
  if (it == values.end()) {
    diagnostics.error("M005", "missing '" + std::string(key) + "' in section '" +
                                   std::string(section) + "'");
    return {};
  }
  return it->second;
}

std::optional<std::uint32_t> optional_u32(
    const std::unordered_map<std::string, std::string>& values,
    std::string_view key, std::string_view section, DiagnosticSink& diagnostics) {
  auto it = values.find(std::string(key));
  if (it == values.end()) return std::nullopt;
  std::uint32_t value = 0;
  if (!parse_u32(it->second, value)) {
    diagnostics.error("M006", "'" + std::string(key) + "' in section '" +
                                   std::string(section) + "' must be an unsigned integer");
    return std::nullopt;
  }
  return value;
}

std::vector<std::uint32_t> parse_id_list(std::string_view text, std::string_view field,
                                         std::string_view section,
                                         DiagnosticSink& diagnostics) {
  std::vector<std::uint32_t> result;
  if (trim_ascii(text).empty()) return result;
  for (const std::string& item : split_ascii(text, ',', false)) {
    std::uint32_t value = 0;
    if (!parse_u32(trim_ascii(item), value)) {
      diagnostics.error("M007", "invalid id in '" + std::string(field) + "' for section '" +
                                     std::string(section) + "'");
      continue;
    }
    result.push_back(value);
  }
  return result;
}

std::string quote_value(std::string_view value) {
  bool plain = !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
    return c != '\n' && c != '\r' && c != '#' && c != '[' && c != ']';
  });
  if (plain) return trim_ascii(value);
  return '"' + json_escape(value) + '"';
}

}  // namespace

Result<PackageManifest> parse_manifest_text(std::string_view text,
                                            DiagnosticSink* diagnostics,
                                            std::string source_name) {
  DiagnosticSink local;
  DiagnosticSink& sink = diagnostics != nullptr ? *diagnostics : local;
  Result<PackageManifest> result;
  SourceMap source(std::move(source_name), std::string(text));
  if (!validate_utf8(text)) {
    sink.fatal("M000", "manifest is not valid UTF-8");
    result.status = status_from_diagnostics(sink);
    return result;
  }
  const std::vector<ParsedLine> lines = lex_lines(text, sink, source);
  auto package = collect_section(lines, "package", sink, source);
  const std::string version = require(package, "format", "package", sink);
  if (!version.empty() && !parse_version(version, result.value.format_version))
    sink.error("M010", "package format must be major.minor.patch");
  result.value.package_id = require(package, "id", "package", sink);
  result.value.title = require(package, "title", "package", sink);
  result.value.default_locale = require(package, "default_locale", "package", sink);
  auto revision = optional_u32(package, "revision", "package", sink);
  if (revision) result.value.revision = *revision;
  for (const auto& [key, value] : package) {
    if (key.rfind("property.", 0) == 0) result.value.properties[key.substr(9)] = value;
  }

  for (const std::string& section : section_names(lines, "asset.")) {
    auto values = collect_section(lines, section, sink, source);
    ManifestAsset asset;
    const std::string id_text = section.substr(6);
    if (!parse_u32(id_text, asset.id)) sink.error("M020", "asset section suffix must be a numeric id");
    asset.path = require(values, "path", section, sink);
    asset.media_type = require(values, "type", section, sink);
    auto size = values.find("size");
    if (size != values.end() && !parse_u64(size->second, asset.byte_size))
      sink.error("M021", "asset size must be an unsigned integer");
    auto hash = values.find("hash");
    if (hash != values.end() && !parse_u64(hash->second, asset.content_hash))
      sink.error("M022", "asset hash must be an unsigned integer");
    if (auto width = optional_u32(values, "width", section, sink)) asset.width = *width;
    if (auto height = optional_u32(values, "height", section, sink)) asset.height = *height;
    auto locale = values.find("locale");
    if (locale != values.end()) asset.locale = locale->second;
    auto tags = values.find("tags");
    if (tags != values.end()) {
      for (std::string tag : split_ascii(tags->second, ',', false)) {
        tag = trim_ascii(tag);
        if (!tag.empty()) asset.tags.push_back(std::move(tag));
      }
    }
    result.value.assets.push_back(std::move(asset));
  }

  std::unordered_map<PlaylistId, std::size_t> playlist_indexes;
  for (const std::string& section : section_names(lines, "playlist.")) {
    if (section.find(".item.") != std::string::npos) continue;
    auto values = collect_section(lines, section, sink, source);
    ManifestPlaylist playlist;
    if (!parse_u32(section.substr(9), playlist.id))
      sink.error("M030", "playlist section suffix must be a numeric id");
    playlist.name = require(values, "name", section, sink);
    if (auto root = optional_u32(values, "root", section, sink)) playlist.root_item = *root;
    auto recurrence = values.find("recurrence");
    if (recurrence != values.end()) {
      Result<RecurrenceRule> parsed = parse_recurrence(recurrence->second);
      if (parsed) playlist.recurrence = parsed.value;
      else sink.error("M031", "playlist recurrence is invalid: " + parsed.status.message);
    }
    auto starts = values.find("starts");
    if (starts != values.end()) {
      Result<CivilDateTime> parsed = parse_iso8601(starts->second);
      if (parsed) playlist.starts_at = parsed.value;
      else sink.error("M032", "playlist start time is invalid");
    }
    auto ends = values.find("ends");
    if (ends != values.end()) {
      Result<CivilDateTime> parsed = parse_iso8601(ends->second);
      if (parsed) playlist.ends_at = parsed.value;
      else sink.error("M033", "playlist end time is invalid");
    }
    playlist_indexes[playlist.id] = result.value.playlists.size();
    result.value.playlists.push_back(std::move(playlist));
  }

  for (const std::string& section : section_names(lines, "playlist.")) {
    const std::size_t marker = section.find(".item.");
    if (marker == std::string::npos) continue;
    std::uint32_t playlist_id = 0;
    std::uint32_t item_id = 0;
    if (!parse_u32(section.substr(9, marker - 9), playlist_id) ||
        !parse_u32(section.substr(marker + 6), item_id)) {
      sink.error("M034", "playlist item section requires numeric playlist and item ids");
      continue;
    }
    auto playlist = playlist_indexes.find(playlist_id);
    if (playlist == playlist_indexes.end()) {
      sink.error("M035", "playlist item references a missing playlist section");
      continue;
    }
    auto values = collect_section(lines, section, sink, source);
    ManifestPlaylistItem item;
    item.id = item_id;
    if (auto asset = optional_u32(values, "asset", section, sink)) item.asset_id = *asset;
    if (auto duration = optional_u32(values, "duration_ms", section, sink)) item.duration_ms = *duration;
    auto transition = values.find("transition");
    item.transition = transition == values.end() ? "cut" : transition->second;
    item.next_item = optional_u32(values, "next", section, sink);
    for (const auto& [key, value] : values) {
      if (key.rfind("meta.", 0) == 0) item.metadata[key.substr(5)] = value;
    }
    result.value.playlists[playlist->second].items.push_back(std::move(item));
  }

  for (const std::string& section : section_names(lines, "group.")) {
    auto values = collect_section(lines, section, sink, source);
    ManifestDeviceGroup group;
    if (!parse_u32(section.substr(6), group.id)) sink.error("M040", "group section suffix must be a numeric id");
    group.name = require(values, "name", section, sink);
    if (auto capabilities = optional_u32(values, "capabilities", section, sink))
      group.required_capabilities = *capabilities;
    auto devices = values.find("devices");
    if (devices != values.end()) group.devices = parse_id_list(devices->second, "devices", section, sink);
    auto playlists = values.find("playlists");
    if (playlists != values.end()) group.playlists = parse_id_list(playlists->second, "playlists", section, sink);
    result.value.device_groups.push_back(std::move(group));
  }

  Status validation = validate_manifest(result.value, &sink);
  result.status = validation;
  return result;
}

Status validate_manifest(const PackageManifest& manifest, DiagnosticSink* diagnostics) {
  DiagnosticSink local;
  DiagnosticSink& sink = diagnostics != nullptr ? *diagnostics : local;
  if (manifest.format_version.major != 1)
    sink.error("V001", "unsupported manifest major version");
  if (manifest.package_id.empty() || manifest.package_id.size() > 128)
    sink.error("V002", "package id must contain 1 to 128 characters");
  if (manifest.title.empty()) sink.error("V003", "package title cannot be empty");
  Result<LocaleTag> default_locale = parse_locale_tag(manifest.default_locale);
  if (!default_locale) sink.error("V004", "default locale is not a valid language tag");

  std::unordered_set<AssetId> asset_ids;
  for (const ManifestAsset& asset : manifest.assets) {
    if (asset.id == 0 || !asset_ids.insert(asset.id).second)
      sink.error("V010", "asset ids must be unique and non-zero");
    if (asset.path.empty() || asset.path.front() == '/' || asset.path.find("..") != std::string::npos)
      sink.error("V011", "asset paths must be relative and cannot contain '..'");
    if (asset.media_type.empty() || asset.media_type.find('/') == std::string::npos)
      sink.error("V012", "asset media type must contain a slash");
    if ((asset.width == 0) != (asset.height == 0))
      sink.error("V013", "asset dimensions must either both be present or both be absent");
    if (!asset.locale.empty() && !parse_locale_tag(asset.locale))
      sink.error("V014", "asset locale is invalid");
  }

  std::unordered_set<PlaylistId> playlist_ids;
  for (const ManifestPlaylist& playlist : manifest.playlists) {
    if (playlist.id == 0 || !playlist_ids.insert(playlist.id).second)
      sink.error("V020", "playlist ids must be unique and non-zero");
    if (playlist.name.empty()) sink.error("V021", "playlist name cannot be empty");
    if (playlist.starts_at && playlist.ends_at &&
        unix_seconds(*playlist.starts_at) >= unix_seconds(*playlist.ends_at))
      sink.error("V022", "playlist end must be after its start");
    std::unordered_set<std::uint32_t> item_ids;
    bool root_found = playlist.root_item == 0 && playlist.items.empty();
    for (const ManifestPlaylistItem& item : playlist.items) {
      if (item.id == 0 || !item_ids.insert(item.id).second)
        sink.error("V023", "playlist item ids must be unique and non-zero");
      if (item.id == playlist.root_item) root_found = true;
      if (asset_ids.find(item.asset_id) == asset_ids.end())
        sink.error("V024", "playlist item references an unknown asset");
      if (item.duration_ms == 0 || item.duration_ms > 24u * 60u * 60u * 1000u)
        sink.error("V025", "playlist item duration is outside supported bounds");
      if (item.transition != "cut" && item.transition != "fade" &&
          item.transition != "wipe" && item.transition != "crossfade")
        sink.error("V026", "playlist item transition is unsupported");
    }
    if (!root_found) sink.error("V027", "playlist root item does not exist");
    for (const ManifestPlaylistItem& item : playlist.items) {
      if (item.next_item && item_ids.find(*item.next_item) == item_ids.end())
        sink.error("V028", "playlist next item reference does not exist");
    }
  }

  std::unordered_set<std::uint32_t> group_ids;
  for (const ManifestDeviceGroup& group : manifest.device_groups) {
    if (group.id == 0 || !group_ids.insert(group.id).second)
      sink.error("V030", "device group ids must be unique and non-zero");
    if (group.name.empty()) sink.error("V031", "device group name cannot be empty");
    std::unordered_set<DeviceId> unique_devices;
    for (DeviceId device : group.devices) {
      if (device == 0 || !unique_devices.insert(device).second)
        sink.warning("V032", "device group contains a duplicate or zero device id");
    }
    for (PlaylistId playlist : group.playlists) {
      if (playlist_ids.find(playlist) == playlist_ids.end())
        sink.error("V033", "device group references an unknown playlist");
    }
  }
  return status_from_diagnostics(sink);
}

std::string serialize_manifest_text(const PackageManifest& manifest) {
  std::ostringstream out;
  out << "[package]\n";
  out << "format=" << manifest.format_version.major << '.' << manifest.format_version.minor << '.'
      << manifest.format_version.patch << '\n';
  out << "id=" << quote_value(manifest.package_id) << '\n';
  out << "title=" << quote_value(manifest.title) << '\n';
  out << "default_locale=" << manifest.default_locale << '\n';
  out << "revision=" << manifest.revision << '\n';
  std::vector<std::pair<std::string, std::string>> properties(manifest.properties.begin(), manifest.properties.end());
  std::sort(properties.begin(), properties.end());
  for (const auto& [key, value] : properties) out << "property." << key << '=' << quote_value(value) << '\n';

  for (const ManifestAsset& asset : manifest.assets) {
    out << "\n[asset." << asset.id << "]\n";
    out << "path=" << quote_value(asset.path) << '\n';
    out << "type=" << asset.media_type << '\n';
    out << "size=" << asset.byte_size << '\n';
    out << "hash=" << asset.content_hash << '\n';
    if (asset.width != 0) out << "width=" << asset.width << "\nheight=" << asset.height << '\n';
    if (!asset.locale.empty()) out << "locale=" << asset.locale << '\n';
    if (!asset.tags.empty()) out << "tags=" << join_strings(asset.tags, ",") << '\n';
  }
  for (const ManifestPlaylist& playlist : manifest.playlists) {
    out << "\n[playlist." << playlist.id << "]\n";
    out << "name=" << quote_value(playlist.name) << '\n';
    out << "root=" << playlist.root_item << '\n';
    if (playlist.starts_at) out << "starts=" << format_iso8601(*playlist.starts_at) << '\n';
    if (playlist.ends_at) out << "ends=" << format_iso8601(*playlist.ends_at) << '\n';
    for (const ManifestPlaylistItem& item : playlist.items) {
      out << "\n[playlist." << playlist.id << ".item." << item.id << "]\n";
      out << "asset=" << item.asset_id << '\n';
      out << "duration_ms=" << item.duration_ms << '\n';
      out << "transition=" << item.transition << '\n';
      if (item.next_item) out << "next=" << *item.next_item << '\n';
      std::vector<std::pair<std::string, std::string>> metadata(item.metadata.begin(), item.metadata.end());
      std::sort(metadata.begin(), metadata.end());
      for (const auto& [key, value] : metadata) out << "meta." << key << '=' << quote_value(value) << '\n';
    }
  }
  for (const ManifestDeviceGroup& group : manifest.device_groups) {
    out << "\n[group." << group.id << "]\n";
    out << "name=" << quote_value(group.name) << '\n';
    out << "capabilities=" << group.required_capabilities << '\n';
    std::vector<std::string> devices;
    for (DeviceId id : group.devices) devices.push_back(std::to_string(id));
    std::vector<std::string> playlists;
    for (PlaylistId id : group.playlists) playlists.push_back(std::to_string(id));
    out << "devices=" << join_strings(devices, ",") << '\n';
    out << "playlists=" << join_strings(playlists, ",") << '\n';
  }
  return out.str();
}

std::uint64_t manifest_digest(const PackageManifest& manifest) {
  const std::string serialized = serialize_manifest_text(manifest);
  return stable_text_hash(serialized);
}

std::vector<AssetId> referenced_assets(const PackageManifest& manifest) {
  std::vector<AssetId> result;
  for (const ManifestPlaylist& playlist : manifest.playlists) {
    for (const ManifestPlaylistItem& item : playlist.items) result.push_back(item.asset_id);
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<PlaylistId> playlists_for_device(const PackageManifest& manifest,
                                             DeviceId device,
                                             std::uint32_t capabilities) {
  std::vector<PlaylistId> result;
  for (const ManifestDeviceGroup& group : manifest.device_groups) {
    if ((capabilities & group.required_capabilities) != group.required_capabilities) continue;
    if (std::find(group.devices.begin(), group.devices.end(), device) == group.devices.end()) continue;
    result.insert(result.end(), group.playlists.begin(), group.playlists.end());
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

}  // namespace lanternstage

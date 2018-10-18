#include "lanternstage/deployment_plan.h"

#include "lanternstage/text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>

namespace lanternstage {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint64_t kKib = 1024ULL;
constexpr Tick kDaySeconds = 24ULL * 60ULL * 60ULL;

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= kFnvPrime;
  }
}

template <typename T>
void hash_value(std::uint64_t& hash, const T& value) {
  hash_bytes(hash, &value, sizeof(value));
}

void hash_text(std::uint64_t& hash, const std::string& text) {
  const std::uint64_t size = text.size();
  hash_value(hash, size);
  hash_bytes(hash, text.data(), text.size());
}

std::string json_escape(const std::string& value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20U) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<unsigned>(c) << std::dec;
        } else {
          out << static_cast<char>(c);
        }
    }
  }
  return out.str();
}

bool locale_matches(const std::string& requested,
                    const std::string& available,
                    bool fallback) {
  if (requested.empty() || available.empty()) {
    return true;
  }
  Result<LocaleTag> parsed_requested = parse_locale_tag(requested);
  Result<LocaleTag> parsed_available = parse_locale_tag(available);
  const std::string a = parsed_requested ? parsed_requested.value.canonical() : ascii_lower(requested);
  const std::string b = parsed_available ? parsed_available.value.canonical() : ascii_lower(available);
  if (a == b) {
    return true;
  }
  if (!fallback) {
    return false;
  }
  const std::size_t a_sep = a.find('-');
  const std::size_t b_sep = b.find('-');
  const std::string a_language = a.substr(0, a_sep);
  const std::string b_language = b.substr(0, b_sep);
  return !a_language.empty() && a_language == b_language;
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

Tick saturating_tick_add(Tick left, Tick right) {
  if (right > std::numeric_limits<Tick>::max() - left) {
    return std::numeric_limits<Tick>::max();
  }
  return left + right;
}

const PlanningDevice* find_device(const std::vector<PlanningDevice>& devices,
                                  DeviceId id) {
  auto it = std::find_if(devices.begin(), devices.end(), [id](const PlanningDevice& value) {
    return value.id == id;
  });
  return it == devices.end() ? nullptr : &*it;
}

const ManifestAsset* find_asset(const PackageManifest& manifest, AssetId id) {
  auto it = std::find_if(manifest.assets.begin(), manifest.assets.end(), [id](const ManifestAsset& value) {
    return value.id == id;
  });
  return it == manifest.assets.end() ? nullptr : &*it;
}

const ManifestPlaylist* find_playlist(const PackageManifest& manifest, PlaylistId id) {
  auto it = std::find_if(manifest.playlists.begin(), manifest.playlists.end(), [id](const ManifestPlaylist& value) {
    return value.id == id;
  });
  return it == manifest.playlists.end() ? nullptr : &*it;
}

std::uint64_t margin_bytes(std::uint64_t bytes, std::uint32_t percent) {
  const std::uint64_t whole = bytes / 100ULL;
  const std::uint64_t remainder = bytes % 100ULL;
  const std::uint64_t margin = saturating_add(whole * percent,
      (remainder * percent + 99ULL) / 100ULL);
  return saturating_add(bytes, margin);
}

PlanIssue make_issue(PlanSeverity severity,
                     std::string code,
                     std::string message,
                     std::optional<DeviceId> device = std::nullopt,
                     std::optional<AssetId> asset = std::nullopt,
                     std::optional<PlaylistId> playlist = std::nullopt) {
  PlanIssue issue;
  issue.severity = severity;
  issue.code = std::move(code);
  issue.message = std::move(message);
  issue.device_id = device;
  issue.asset_id = asset;
  issue.playlist_id = playlist;
  return issue;
}

}  // namespace

bool DeploymentPlan::feasible() const {
  if (std::any_of(issues.begin(), issues.end(), [](const PlanIssue& issue) {
        return issue.severity == PlanSeverity::error;
      })) {
    return false;
  }
  return std::all_of(activations.begin(), activations.end(), [](const DeviceActivation& activation) {
    return activation.feasible;
  });
}

DeploymentPlanner::DeploymentPlanner() = default;

void DeploymentPlanner::set_policy(PlanningPolicy policy) {
  policy.maximum_parallel_downloads = std::max<std::uint32_t>(1, policy.maximum_parallel_downloads);
  policy.maximum_wave_duration_seconds = std::max<std::uint32_t>(60, policy.maximum_wave_duration_seconds);
  policy.safety_margin_percent = std::min<std::uint32_t>(100, policy.safety_margin_percent);
  policy.preferred_start_minute %= (24U * 60U);
  policy_ = policy;
}

const PlanningPolicy& DeploymentPlanner::policy() const {
  return policy_;
}

void DeploymentPlanner::set_inventory(std::vector<DeviceInventoryEntry> entries) {
  inventory_.clear();
  for (const DeviceInventoryEntry& entry : entries) {
    if (entry.device_id == 0 || entry.asset_id == 0) {
      continue;
    }
    inventory_[entry.device_id][entry.asset_id] = entry;
  }
}

void DeploymentPlanner::clear_inventory() {
  inventory_.clear();
}

std::vector<PlaylistId> DeploymentPlanner::select_playlists(
    const PackageManifest& manifest,
    const PlanningDevice& device,
    std::vector<PlanIssue>& issues) const {
  std::vector<PlaylistId> selected = playlists_for_device(manifest, device.id, device.capabilities);
  std::sort(selected.begin(), selected.end());
  selected.erase(std::unique(selected.begin(), selected.end()), selected.end());

  for (PlaylistId id : selected) {
    if (find_playlist(manifest, id) == nullptr) {
      issues.push_back(make_issue(PlanSeverity::error,
                                  "planner.playlist.missing",
                                  "device group refers to a playlist that is absent from the manifest",
                                  device.id,
                                  std::nullopt,
                                  id));
    }
  }
  if (selected.empty()) {
    issues.push_back(make_issue(PlanSeverity::warning,
                                "planner.device.unassigned",
                                "no playlist is assigned to this device",
                                device.id));
  }
  return selected;
}

std::vector<const ManifestAsset*> DeploymentPlanner::select_assets(
    const PackageManifest& manifest,
    const std::vector<PlaylistId>& playlists,
    const PlanningDevice& device,
    std::vector<PlanIssue>& issues) const {
  std::unordered_set<AssetId> selected_ids;
  std::vector<const ManifestAsset*> selected;

  for (PlaylistId playlist_id : playlists) {
    const ManifestPlaylist* playlist = find_playlist(manifest, playlist_id);
    if (playlist == nullptr) {
      continue;
    }
    for (const ManifestPlaylistItem& item : playlist->items) {
      const ManifestAsset* asset = find_asset(manifest, item.asset_id);
      if (asset == nullptr) {
        issues.push_back(make_issue(PlanSeverity::error,
                                    "planner.asset.missing",
                                    "playlist item refers to an asset that is absent from the manifest",
                                    device.id,
                                    item.asset_id,
                                    playlist_id));
        continue;
      }
      if (!locale_matches(device.locale, asset->locale, policy_.allow_locale_fallback)) {
        issues.push_back(make_issue(PlanSeverity::warning,
                                    "planner.asset.locale",
                                    "asset locale does not match the device locale",
                                    device.id,
                                    asset->id,
                                    playlist_id));
      }
      if (asset->width != 0 && asset->height != 0 &&
          device.viewport_width != 0 && device.viewport_height != 0 &&
          (asset->width > device.viewport_width || asset->height > device.viewport_height)) {
        const PlanSeverity severity = policy_.allow_downscale ? PlanSeverity::information
                                                              : PlanSeverity::error;
        issues.push_back(make_issue(severity,
                                    "planner.asset.viewport",
                                    policy_.allow_downscale
                                        ? "asset will be downscaled for the device viewport"
                                        : "asset exceeds the device viewport and downscaling is disabled",
                                    device.id,
                                    asset->id,
                                    playlist_id));
      }
      if (selected_ids.insert(asset->id).second) {
        selected.push_back(asset);
      }
    }
  }

  std::sort(selected.begin(), selected.end(), [](const ManifestAsset* left,
                                                  const ManifestAsset* right) {
    if (left->byte_size != right->byte_size) {
      return left->byte_size > right->byte_size;
    }
    return left->id < right->id;
  });
  return selected;
}

bool DeploymentPlanner::inventory_has(DeviceId device, const ManifestAsset& asset) const {
  const auto device_it = inventory_.find(device);
  if (device_it == inventory_.end()) {
    return false;
  }
  const auto asset_it = device_it->second.find(asset.id);
  if (asset_it == device_it->second.end()) {
    return false;
  }
  return asset_it->second.content_hash == asset.content_hash &&
         asset_it->second.byte_size == asset.byte_size;
}

Tick DeploymentPlanner::estimate_transfer_seconds(const PlanningDevice& device,
                                                  std::uint64_t bytes) const {
  if (bytes == 0) {
    return 0;
  }
  if (device.network == NetworkClass::offline || device.bandwidth_kib_per_second == 0) {
    return std::numeric_limits<Tick>::max();
  }
  const std::uint64_t bytes_per_second =
      static_cast<std::uint64_t>(device.bandwidth_kib_per_second) * kKib;
  return static_cast<Tick>((bytes + bytes_per_second - 1ULL) / bytes_per_second);
}

void DeploymentPlanner::append_storage_issues(const PlanningDevice& device,
                                              std::uint64_t required_bytes,
                                              std::vector<PlanIssue>& issues,
                                              bool& feasible) const {
  std::uint64_t reserved = margin_bytes(required_bytes, policy_.safety_margin_percent);
  if (policy_.retain_previous_package) {
    reserved = saturating_add(reserved, required_bytes);
  }
  if (device.storage_capacity != 0 && reserved > device.storage_capacity) {
    issues.push_back(make_issue(PlanSeverity::error,
                                "planner.storage.capacity",
                                "the package cannot fit within the device storage capacity",
                                device.id));
    feasible = false;
  } else if (reserved > device.storage_available) {
    issues.push_back(make_issue(PlanSeverity::error,
                                "planner.storage.available",
                                "the package cannot fit within currently available device storage",
                                device.id));
    feasible = false;
  } else if (device.storage_available != 0 && reserved * 5ULL > device.storage_available * 4ULL) {
    issues.push_back(make_issue(PlanSeverity::warning,
                                "planner.storage.pressure",
                                "deployment will consume more than eighty percent of available storage",
                                device.id));
  }
}

Tick DeploymentPlanner::choose_activation_time(Tick now,
                                               Tick requested,
                                               const std::vector<DeviceWork>& work,
                                               std::vector<PlanIssue>& issues) const {
  Tick longest = 0;
  for (const DeviceWork& item : work) {
    if (!item.feasible || item.duration_seconds == std::numeric_limits<Tick>::max()) {
      continue;
    }
    longest = std::max(longest, item.duration_seconds);
  }

  const std::uint64_t wave_count =
      (work.size() + policy_.maximum_parallel_downloads - 1ULL) /
      policy_.maximum_parallel_downloads;
  const Tick transfer_window = longest == 0 ? 0 : longest * std::max<std::uint64_t>(1, wave_count);
  const Tick earliest = saturating_tick_add(now, transfer_window + 30ULL);

  if (requested != 0) {
    if (requested < earliest) {
      issues.push_back(make_issue(PlanSeverity::warning,
                                  "planner.activation.deferred",
                                  "requested activation does not leave enough time for all transfers"));
      return earliest;
    }
    return requested;
  }

  const Tick day = now / kDaySeconds;
  Tick candidate = day * kDaySeconds +
      static_cast<Tick>(policy_.preferred_start_minute) * 60ULL;
  if (candidate < earliest) {
    candidate = saturating_tick_add(candidate, kDaySeconds);
  }
  return candidate;
}

void DeploymentPlanner::assign_waves(std::vector<DeviceWork>& work,
                                     Tick activation,
                                     DeploymentPlan& plan) const {
  std::stable_sort(work.begin(), work.end(), [](const DeviceWork& left, const DeviceWork& right) {
    if (left.feasible != right.feasible) {
      return left.feasible > right.feasible;
    }
    if (left.duration_seconds != right.duration_seconds) {
      return left.duration_seconds > right.duration_seconds;
    }
    return left.device->id < right.device->id;
  });

  struct WaveState {
    DeploymentWave wave;
    Tick duration{0};
  };
  std::vector<WaveState> states;

  for (const DeviceWork& item : work) {
    DeviceActivation activation_item;
    activation_item.device_id = item.device->id;
    activation_item.playlists = item.playlists;
    activation_item.transfer_bytes = item.transfer_bytes;
    activation_item.activation_time = activation;
    activation_item.feasible = item.feasible;

    std::uint32_t sequence = 0;
    for (const ManifestAsset* asset : item.assets) {
      AssetTransfer transfer;
      transfer.asset_id = asset->id;
      transfer.path = asset->path;
      transfer.bytes = asset->byte_size;
      transfer.content_hash = asset->content_hash;
      transfer.reused = inventory_has(item.device->id, *asset);
      transfer.required = true;
      transfer.sequence = sequence++;
      activation_item.transfers.push_back(std::move(transfer));
    }

    if (!item.feasible || item.duration_seconds == std::numeric_limits<Tick>::max()) {
      activation_item.wave = std::numeric_limits<std::uint32_t>::max();
      plan.activations.push_back(std::move(activation_item));
      continue;
    }

    std::size_t chosen = states.size();
    Tick best_duration = std::numeric_limits<Tick>::max();
    for (std::size_t i = 0; i < states.size(); ++i) {
      WaveState& state = states[i];
      if (state.wave.devices.size() >= policy_.maximum_parallel_downloads) {
        continue;
      }
      const Tick duration = std::max(state.duration, item.duration_seconds);
      if (duration > policy_.maximum_wave_duration_seconds) {
        continue;
      }
      if (duration < best_duration) {
        chosen = i;
        best_duration = duration;
      }
    }
    if (chosen == states.size()) {
      WaveState state;
      state.wave.index = static_cast<std::uint32_t>(states.size());
      state.duration = item.duration_seconds;
      states.push_back(std::move(state));
    } else {
      states[chosen].duration = std::max(states[chosen].duration, item.duration_seconds);
    }

    WaveState& state = states[chosen];
    state.wave.devices.push_back(item.device->id);
    state.wave.transfer_bytes = saturating_add(state.wave.transfer_bytes, item.transfer_bytes);
    activation_item.wave = state.wave.index;
    plan.activations.push_back(std::move(activation_item));
  }

  Tick cursor = activation;
  for (auto it = states.rbegin(); it != states.rend(); ++it) {
    cursor = cursor > it->duration ? cursor - it->duration : 0;
    it->wave.starts_at = cursor;
    it->wave.finishes_at = saturating_tick_add(cursor, it->duration);
  }
  std::reverse(states.begin(), states.end());
  std::sort(states.begin(), states.end(), [](const WaveState& left, const WaveState& right) {
    return left.wave.starts_at < right.wave.starts_at;
  });

  std::unordered_map<std::uint32_t, std::pair<Tick, Tick>> wave_times;
  for (std::size_t i = 0; i < states.size(); ++i) {
    states[i].wave.index = static_cast<std::uint32_t>(i);
    wave_times[states[i].wave.index] = {states[i].wave.starts_at, states[i].wave.finishes_at};
    plan.waves.push_back(states[i].wave);
  }

  for (DeviceActivation& item : plan.activations) {
    if (item.wave == std::numeric_limits<std::uint32_t>::max()) {
      continue;
    }
    auto found = std::find_if(plan.waves.begin(), plan.waves.end(), [&item](const DeploymentWave& wave) {
      return std::find(wave.devices.begin(), wave.devices.end(), item.device_id) != wave.devices.end();
    });
    if (found != plan.waves.end()) {
      item.wave = found->index;
      item.download_start = found->starts_at;
      item.download_finish = found->finishes_at;
    }
  }
}

Result<DeploymentPlan> DeploymentPlanner::create_plan(
    const PackageManifest& manifest,
    const std::vector<PlanningDevice>& devices,
    Tick now,
    Tick requested_activation) const {
  Result<DeploymentPlan> result;
  result.status = Status::success();
  result.value.package_id = manifest.package_id;
  result.value.revision = manifest.revision;
  result.value.created_at = now;

  DiagnosticSink diagnostics;
  Status manifest_status = validate_manifest(manifest, &diagnostics);
  if (!manifest_status) {
    result.status = Status::failure(Status::Code::malformed,
                                    "cannot plan an invalid package manifest");
    for (const Diagnostic& diagnostic : diagnostics.diagnostics()) {
      result.value.issues.push_back(make_issue(
          diagnostic.severity == Severity::error || diagnostic.severity == Severity::fatal ? PlanSeverity::error
                                                            : PlanSeverity::warning,
          diagnostic.code,
          diagnostic.message));
    }
    return result;
  }

  if (devices.empty()) {
    result.status = Status::failure(Status::Code::not_found,
                                    "no deployment devices were supplied");
    result.value.issues.push_back(make_issue(PlanSeverity::error,
                                             "planner.devices.empty",
                                             "a deployment plan requires at least one device"));
    return result;
  }

  std::unordered_set<DeviceId> seen_devices;
  std::vector<DeviceWork> work;
  work.reserve(devices.size());

  for (const PlanningDevice& device : devices) {
    DeviceWork item;
    item.device = &device;
    if (device.id == 0 || !seen_devices.insert(device.id).second) {
      result.value.issues.push_back(make_issue(PlanSeverity::error,
                                               "planner.device.identity",
                                               "device identifiers must be nonzero and unique",
                                               device.id));
      item.feasible = false;
    }
    if (!device.accepts_updates) {
      result.value.issues.push_back(make_issue(PlanSeverity::error,
                                               "planner.device.locked",
                                               "device is not currently accepting package updates",
                                               device.id));
      item.feasible = false;
    }
    if (device.available_until != 0 && device.available_until <= now) {
      result.value.issues.push_back(make_issue(PlanSeverity::error,
                                               "planner.device.unavailable",
                                               "device availability window has already ended",
                                               device.id));
      item.feasible = false;
    }

    item.playlists = select_playlists(manifest, device, result.value.issues);
    item.assets = select_assets(manifest, item.playlists, device, result.value.issues);
    for (const ManifestAsset* asset : item.assets) {
      if (!inventory_has(device.id, *asset)) {
        item.transfer_bytes = saturating_add(item.transfer_bytes, asset->byte_size);
      }
    }
    append_storage_issues(device, item.transfer_bytes, result.value.issues, item.feasible);

    if (device.network == NetworkClass::offline && item.transfer_bytes != 0) {
      result.value.issues.push_back(make_issue(PlanSeverity::error,
                                               "planner.network.offline",
                                               "device requires assets but has no transfer channel",
                                               device.id));
      item.feasible = false;
    }
    if (device.network == NetworkClass::metered && policy_.metered_budget_kib != 0 &&
        item.transfer_bytes > static_cast<std::uint64_t>(policy_.metered_budget_kib) * kKib) {
      result.value.issues.push_back(make_issue(PlanSeverity::error,
                                               "planner.network.budget",
                                               "device transfer exceeds the configured metered-network budget",
                                               device.id));
      item.feasible = false;
    }

    item.duration_seconds = estimate_transfer_seconds(device, item.transfer_bytes);
    if (item.duration_seconds != std::numeric_limits<Tick>::max() &&
        item.duration_seconds > policy_.maximum_wave_duration_seconds) {
      result.value.issues.push_back(make_issue(PlanSeverity::warning,
                                               "planner.network.slow",
                                               "device transfer is longer than the preferred deployment wave",
                                               device.id));
    }
    result.value.total_transfer_bytes =
        saturating_add(result.value.total_transfer_bytes, item.transfer_bytes);
    work.push_back(std::move(item));
  }

  result.value.activation_time = choose_activation_time(now,
                                                        requested_activation,
                                                        work,
                                                        result.value.issues);
  assign_waves(work, result.value.activation_time, result.value);

  for (DeviceActivation& activation : result.value.activations) {
    const PlanningDevice* device = find_device(devices, activation.device_id);
    if (device == nullptr) {
      continue;
    }
    if (device->available_from != 0 && activation.download_start < device->available_from) {
      result.value.issues.push_back(make_issue(PlanSeverity::error,
                                               "planner.window.starts",
                                               "download starts before the device availability window",
                                               device->id));
      activation.feasible = false;
    }
    if (device->available_until != 0 && activation.download_finish > device->available_until) {
      result.value.issues.push_back(make_issue(PlanSeverity::error,
                                               "planner.window.ends",
                                               "download ends after the device availability window",
                                               device->id));
      activation.feasible = false;
    }
  }

  result.value.digest = compute_digest(result.value);
  return result;
}

Status DeploymentPlanner::validate_plan(const DeploymentPlan& plan,
                                        const PackageManifest& manifest,
                                        const std::vector<PlanningDevice>& devices,
                                        std::vector<PlanIssue>* issues) const {
  std::vector<PlanIssue> local;
  auto report = [&local](PlanIssue issue) { local.push_back(std::move(issue)); };

  if (plan.package_id != manifest.package_id || plan.revision != manifest.revision) {
    report(make_issue(PlanSeverity::error,
                      "planner.plan.package",
                      "deployment plan does not identify the supplied package revision"));
  }
  if (plan.activation_time < plan.created_at) {
    report(make_issue(PlanSeverity::error,
                      "planner.plan.time",
                      "activation precedes plan creation"));
  }

  std::unordered_set<DeviceId> activated;
  std::uint64_t total = 0;
  for (const DeviceActivation& activation : plan.activations) {
    if (!activated.insert(activation.device_id).second) {
      report(make_issue(PlanSeverity::error,
                        "planner.plan.duplicate-device",
                        "device appears more than once in the plan",
                        activation.device_id));
    }
    const PlanningDevice* device = find_device(devices, activation.device_id);
    if (device == nullptr) {
      report(make_issue(PlanSeverity::error,
                        "planner.plan.unknown-device",
                        "plan contains a device that is absent from the supplied inventory",
                        activation.device_id));
      continue;
    }
    if (activation.download_finish < activation.download_start ||
        activation.activation_time < activation.download_finish) {
      report(make_issue(PlanSeverity::error,
                        "planner.plan.order",
                        "device transfer and activation times are not ordered",
                        activation.device_id));
    }

    std::unordered_set<AssetId> transfers;
    std::uint64_t bytes = 0;
    for (const AssetTransfer& transfer : activation.transfers) {
      if (!transfers.insert(transfer.asset_id).second) {
        report(make_issue(PlanSeverity::error,
                          "planner.plan.duplicate-asset",
                          "device transfer list contains an asset more than once",
                          activation.device_id,
                          transfer.asset_id));
      }
      const ManifestAsset* asset = find_asset(manifest, transfer.asset_id);
      if (asset == nullptr || asset->content_hash != transfer.content_hash ||
          asset->byte_size != transfer.bytes) {
        report(make_issue(PlanSeverity::error,
                          "planner.plan.asset-mismatch",
                          "device transfer does not match the package manifest",
                          activation.device_id,
                          transfer.asset_id));
      }
      if (!transfer.reused) {
        bytes = saturating_add(bytes, transfer.bytes);
      }
    }
    if (bytes != activation.transfer_bytes) {
      report(make_issue(PlanSeverity::error,
                        "planner.plan.byte-count",
                        "device transfer byte total is inconsistent",
                        activation.device_id));
    }
    total = saturating_add(total, bytes);

    for (PlaylistId playlist : activation.playlists) {
      if (find_playlist(manifest, playlist) == nullptr) {
        report(make_issue(PlanSeverity::error,
                          "planner.plan.playlist",
                          "device activation refers to an unknown playlist",
                          activation.device_id,
                          std::nullopt,
                          playlist));
      }
    }
  }

  if (total != plan.total_transfer_bytes) {
    report(make_issue(PlanSeverity::error,
                      "planner.plan.total",
                      "plan transfer total is inconsistent"));
  }
  if (compute_digest(plan) != plan.digest) {
    report(make_issue(PlanSeverity::error,
                      "planner.plan.digest",
                      "plan digest does not match its contents"));
  }

  for (const DeploymentWave& wave : plan.waves) {
    if (wave.finishes_at < wave.starts_at) {
      report(make_issue(PlanSeverity::error,
                        "planner.wave.order",
                        "deployment wave ends before it starts"));
    }
    if (wave.devices.size() > policy_.maximum_parallel_downloads) {
      report(make_issue(PlanSeverity::error,
                        "planner.wave.parallelism",
                        "deployment wave exceeds the configured parallel download limit"));
    }
  }

  if (issues != nullptr) {
    issues->insert(issues->end(), local.begin(), local.end());
  }
  if (std::any_of(local.begin(), local.end(), [](const PlanIssue& issue) {
        return issue.severity == PlanSeverity::error;
      })) {
    return Status::failure(Status::Code::malformed, "deployment plan validation failed");
  }
  return Status::success();
}

std::uint64_t DeploymentPlanner::compute_digest(const DeploymentPlan& plan) const {
  std::uint64_t hash = kFnvOffset;
  hash_text(hash, plan.package_id);
  hash_value(hash, plan.revision);
  hash_value(hash, plan.created_at);
  hash_value(hash, plan.activation_time);
  hash_value(hash, plan.total_transfer_bytes);

  for (const DeviceActivation& activation : plan.activations) {
    hash_value(hash, activation.device_id);
    hash_value(hash, activation.transfer_bytes);
    hash_value(hash, activation.download_start);
    hash_value(hash, activation.download_finish);
    hash_value(hash, activation.activation_time);
    hash_value(hash, activation.wave);
    hash_value(hash, activation.feasible);
    for (PlaylistId playlist : activation.playlists) {
      hash_value(hash, playlist);
    }
    for (const AssetTransfer& transfer : activation.transfers) {
      hash_value(hash, transfer.asset_id);
      hash_text(hash, transfer.path);
      hash_value(hash, transfer.bytes);
      hash_value(hash, transfer.content_hash);
      hash_value(hash, transfer.required);
      hash_value(hash, transfer.reused);
      hash_value(hash, transfer.sequence);
    }
  }
  for (const DeploymentWave& wave : plan.waves) {
    hash_value(hash, wave.index);
    hash_value(hash, wave.starts_at);
    hash_value(hash, wave.finishes_at);
    hash_value(hash, wave.transfer_bytes);
    for (DeviceId device : wave.devices) {
      hash_value(hash, device);
    }
  }
  for (const PlanIssue& issue : plan.issues) {
    hash_value(hash, issue.severity);
    hash_text(hash, issue.code);
    hash_text(hash, issue.message);
    hash_value(hash, issue.device_id.value_or(0));
    hash_value(hash, issue.asset_id.value_or(0));
    hash_value(hash, issue.playlist_id.value_or(0));
  }
  return hash;
}

std::string plan_severity_name(PlanSeverity severity) {
  switch (severity) {
    case PlanSeverity::information: return "information";
    case PlanSeverity::warning: return "warning";
    case PlanSeverity::error: return "error";
  }
  return "unknown";
}

std::string network_class_name(NetworkClass network) {
  switch (network) {
    case NetworkClass::offline: return "offline";
    case NetworkClass::metered: return "metered";
    case NetworkClass::standard: return "standard";
    case NetworkClass::high_bandwidth: return "high-bandwidth";
  }
  return "unknown";
}

std::string serialize_deployment_plan(const DeploymentPlan& plan) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"package_id\": \"" << json_escape(plan.package_id) << "\",\n";
  out << "  \"revision\": " << plan.revision << ",\n";
  out << "  \"created_at\": " << plan.created_at << ",\n";
  out << "  \"activation_time\": " << plan.activation_time << ",\n";
  out << "  \"total_transfer_bytes\": " << plan.total_transfer_bytes << ",\n";
  out << "  \"digest\": " << plan.digest << ",\n";
  out << "  \"feasible\": " << (plan.feasible() ? "true" : "false") << ",\n";
  out << "  \"waves\": [\n";
  for (std::size_t i = 0; i < plan.waves.size(); ++i) {
    const DeploymentWave& wave = plan.waves[i];
    out << "    {\"index\": " << wave.index
        << ", \"starts_at\": " << wave.starts_at
        << ", \"finishes_at\": " << wave.finishes_at
        << ", \"transfer_bytes\": " << wave.transfer_bytes
        << ", \"devices\": [";
    for (std::size_t j = 0; j < wave.devices.size(); ++j) {
      if (j != 0) out << ", ";
      out << wave.devices[j];
    }
    out << "]}" << (i + 1 == plan.waves.size() ? "" : ",") << "\n";
  }
  out << "  ],\n";
  out << "  \"activations\": [\n";
  for (std::size_t i = 0; i < plan.activations.size(); ++i) {
    const DeviceActivation& activation = plan.activations[i];
    out << "    {\n";
    out << "      \"device_id\": " << activation.device_id << ",\n";
    out << "      \"wave\": " << activation.wave << ",\n";
    out << "      \"download_start\": " << activation.download_start << ",\n";
    out << "      \"download_finish\": " << activation.download_finish << ",\n";
    out << "      \"activation_time\": " << activation.activation_time << ",\n";
    out << "      \"transfer_bytes\": " << activation.transfer_bytes << ",\n";
    out << "      \"feasible\": " << (activation.feasible ? "true" : "false") << ",\n";
    out << "      \"playlists\": [";
    for (std::size_t j = 0; j < activation.playlists.size(); ++j) {
      if (j != 0) out << ", ";
      out << activation.playlists[j];
    }
    out << "],\n";
    out << "      \"transfers\": [\n";
    for (std::size_t j = 0; j < activation.transfers.size(); ++j) {
      const AssetTransfer& transfer = activation.transfers[j];
      out << "        {\"asset_id\": " << transfer.asset_id
          << ", \"path\": \"" << json_escape(transfer.path) << "\""
          << ", \"bytes\": " << transfer.bytes
          << ", \"content_hash\": " << transfer.content_hash
          << ", \"reused\": " << (transfer.reused ? "true" : "false")
          << ", \"sequence\": " << transfer.sequence << "}"
          << (j + 1 == activation.transfers.size() ? "" : ",") << "\n";
    }
    out << "      ]\n";
    out << "    }" << (i + 1 == plan.activations.size() ? "" : ",") << "\n";
  }
  out << "  ],\n";
  out << "  \"issues\": [\n";
  for (std::size_t i = 0; i < plan.issues.size(); ++i) {
    const PlanIssue& issue = plan.issues[i];
    out << "    {\"severity\": \"" << plan_severity_name(issue.severity)
        << "\", \"code\": \"" << json_escape(issue.code)
        << "\", \"message\": \"" << json_escape(issue.message) << "\"";
    if (issue.device_id) out << ", \"device_id\": " << *issue.device_id;
    if (issue.asset_id) out << ", \"asset_id\": " << *issue.asset_id;
    if (issue.playlist_id) out << ", \"playlist_id\": " << *issue.playlist_id;
    out << "}" << (i + 1 == plan.issues.size() ? "" : ",") << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

}  // namespace lanternstage

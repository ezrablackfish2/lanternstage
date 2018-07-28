#pragma once

#include "lanternstage/manifest.h"
#include "lanternstage/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lanternstage {

enum class NetworkClass : std::uint8_t {
  offline = 0,
  metered = 1,
  standard = 2,
  high_bandwidth = 3,
};

enum class PlanSeverity : std::uint8_t {
  information = 0,
  warning = 1,
  error = 2,
};

struct PlanningDevice {
  DeviceId id{0};
  std::string name;
  std::string locale;
  std::uint32_t capabilities{0};
  std::uint64_t storage_capacity{0};
  std::uint64_t storage_available{0};
  std::uint32_t viewport_width{0};
  std::uint32_t viewport_height{0};
  NetworkClass network{NetworkClass::standard};
  std::uint32_t bandwidth_kib_per_second{0};
  Tick available_from{0};
  Tick available_until{0};
  bool accepts_updates{true};
};

struct PlanningPolicy {
  std::uint32_t maximum_parallel_downloads{4};
  std::uint32_t maximum_wave_duration_seconds{900};
  std::uint32_t safety_margin_percent{10};
  std::uint32_t preferred_start_minute{120};
  std::uint32_t metered_budget_kib{0};
  bool allow_locale_fallback{true};
  bool allow_downscale{true};
  bool require_atomic_activation{true};
  bool retain_previous_package{true};
};

struct PlanIssue {
  PlanSeverity severity{PlanSeverity::information};
  std::string code;
  std::string message;
  std::optional<DeviceId> device_id;
  std::optional<AssetId> asset_id;
  std::optional<PlaylistId> playlist_id;
};

struct AssetTransfer {
  AssetId asset_id{0};
  std::string path;
  std::uint64_t bytes{0};
  std::uint64_t content_hash{0};
  bool required{true};
  bool reused{false};
  std::uint32_t sequence{0};
};

struct DeviceActivation {
  DeviceId device_id{0};
  std::vector<PlaylistId> playlists;
  std::vector<AssetTransfer> transfers;
  std::uint64_t transfer_bytes{0};
  Tick download_start{0};
  Tick download_finish{0};
  Tick activation_time{0};
  std::uint32_t wave{0};
  bool feasible{false};
};

struct DeploymentWave {
  std::uint32_t index{0};
  Tick starts_at{0};
  Tick finishes_at{0};
  std::uint64_t transfer_bytes{0};
  std::vector<DeviceId> devices;
};

struct DeploymentPlan {
  std::string package_id;
  Revision revision{0};
  Tick created_at{0};
  Tick activation_time{0};
  std::vector<DeviceActivation> activations;
  std::vector<DeploymentWave> waves;
  std::vector<PlanIssue> issues;
  std::uint64_t total_transfer_bytes{0};
  std::uint64_t digest{0};

  bool feasible() const;
};

struct DeviceInventoryEntry {
  DeviceId device_id{0};
  AssetId asset_id{0};
  std::uint64_t content_hash{0};
  std::uint64_t byte_size{0};
};

class DeploymentPlanner {
 public:
  DeploymentPlanner();

  void set_policy(PlanningPolicy policy);
  const PlanningPolicy& policy() const;

  void set_inventory(std::vector<DeviceInventoryEntry> entries);
  void clear_inventory();

  Result<DeploymentPlan> create_plan(const PackageManifest& manifest,
                                     const std::vector<PlanningDevice>& devices,
                                     Tick now,
                                     Tick requested_activation = 0) const;

  Status validate_plan(const DeploymentPlan& plan,
                       const PackageManifest& manifest,
                       const std::vector<PlanningDevice>& devices,
                       std::vector<PlanIssue>* issues = nullptr) const;

 private:
  struct DeviceWork {
    const PlanningDevice* device{nullptr};
    std::vector<PlaylistId> playlists;
    std::vector<const ManifestAsset*> assets;
    std::uint64_t transfer_bytes{0};
    Tick duration_seconds{0};
    bool feasible{true};
  };

  std::vector<PlaylistId> select_playlists(const PackageManifest& manifest,
                                           const PlanningDevice& device,
                                           std::vector<PlanIssue>& issues) const;
  std::vector<const ManifestAsset*> select_assets(const PackageManifest& manifest,
                                                   const std::vector<PlaylistId>& playlists,
                                                   const PlanningDevice& device,
                                                   std::vector<PlanIssue>& issues) const;
  bool inventory_has(DeviceId device, const ManifestAsset& asset) const;
  Tick estimate_transfer_seconds(const PlanningDevice& device,
                                 std::uint64_t bytes) const;
  Tick choose_activation_time(Tick now,
                              Tick requested,
                              const std::vector<DeviceWork>& work,
                              std::vector<PlanIssue>& issues) const;
  void assign_waves(std::vector<DeviceWork>& work,
                    Tick activation,
                    DeploymentPlan& plan) const;
  void append_storage_issues(const PlanningDevice& device,
                             std::uint64_t required_bytes,
                             std::vector<PlanIssue>& issues,
                             bool& feasible) const;
  std::uint64_t compute_digest(const DeploymentPlan& plan) const;

  PlanningPolicy policy_;
  std::unordered_map<DeviceId, std::unordered_map<AssetId, DeviceInventoryEntry>> inventory_;
};

std::string serialize_deployment_plan(const DeploymentPlan& plan);
std::string plan_severity_name(PlanSeverity severity);
std::string network_class_name(NetworkClass network);

}  // namespace lanternstage

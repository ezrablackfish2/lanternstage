#pragma once

#include "lanternstage/types.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lanternstage {

class WireReader;

class AssetStore {
 public:
  AssetStore();
  ~AssetStore();
  AssetStore(const AssetStore&) = delete;
  AssetStore& operator=(const AssetStore&) = delete;

  Status define_asset(WireReader& payload, std::uint8_t flags);
  Status stage_variant(WireReader& payload, Revision transaction_revision);
  Status drop_unreferenced(WireReader& payload, Epoch epoch);
  Status publish(WireReader& payload, Revision transaction_revision);
  Status create_alias(WireReader& payload);
  Status replace_asset(WireReader& payload, Epoch epoch);
  Status rollback_aliases(WireReader& payload, Revision transaction_revision);
  Status resolve_alias(WireReader& payload);
  Status install_dictionary(WireReader& payload);
  Status rotate_dictionary(WireReader& payload, Epoch epoch);
  Status stage_delta(WireReader& payload, Revision transaction_revision);
  Status materialize_delta(WireReader& payload);
  Status register_font(WireReader& payload);
  Status alias_font(WireReader& payload);
  Status evict_font(WireReader& payload, Epoch epoch);
  Status measure_font(WireReader& payload);

  void begin_transaction(Revision revision);
  void commit_transaction(Revision revision);
  void abort_transaction(Revision revision);
  void reset();

  std::size_t asset_count() const;
  std::size_t dictionary_count() const;
  std::uint64_t observation_hash() const;

 private:
  struct Asset {
    AssetId id{0};
    Revision revision{0};
    Epoch created_epoch{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t reference_count{0};
    std::uint32_t publish_generation{0};
    bool staged{false};
    bool published{false};
    bool tombstoned{false};
    std::string name;
    std::vector<std::uint8_t> bytes;
  };

  struct Alias {
    std::uint32_t alias_id{0};
    AssetId target_id{0};
    Revision bound_revision{0};
    Asset* cached_target{nullptr};
    bool rollback_candidate{false};
  };

  struct Dictionary {
    std::uint32_t id{0};
    Epoch epoch{0};
    std::vector<std::uint8_t> bytes;
    bool retired{false};
  };

  struct DeltaJob {
    std::uint32_t job_id{0};
    AssetId base_asset{0};
    std::uint32_t dictionary_id{0};
    Revision revision{0};
    const Dictionary* dictionary{nullptr};
    std::vector<std::uint8_t> encoded;
    std::size_t expected_size{0};
  };

  struct FontFace {
    std::uint32_t id{0};
    std::uint32_t family{0};
    Epoch last_used{0};
    std::vector<std::int16_t> advances;
    bool aliased{false};
  };

  struct FontAlias {
    std::uint32_t alias_id{0};
    std::uint32_t face_id{0};
    FontFace* cached_face{nullptr};
    Epoch resolved_epoch{0};
  };

  std::unordered_map<AssetId, std::unique_ptr<Asset>> assets_;
  std::unordered_map<std::uint32_t, Alias> aliases_;
  std::unordered_map<std::uint32_t, std::unique_ptr<Dictionary>> dictionaries_;
  std::unordered_map<std::uint32_t, DeltaJob> delta_jobs_;
  std::unordered_map<std::uint32_t, std::unique_ptr<FontFace>> fonts_;
  std::unordered_map<std::uint32_t, FontAlias> font_aliases_;

  std::vector<Asset*> promotion_callbacks_;
  std::vector<Alias*> rollback_aliases_;
  std::vector<FontFace*> font_measure_cache_;
  Revision active_revision_{0};
  std::uint64_t observation_hash_{0};
};

}  // namespace lanternstage

#pragma once

#include "lanternstage/types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lanternstage {
class WireReader;

class PlaylistGraph {
 public:
  PlaylistGraph();
  ~PlaylistGraph();
  PlaylistGraph(const PlaylistGraph&) = delete;
  PlaylistGraph& operator=(const PlaylistGraph&) = delete;

  Status create_playlist(WireReader& payload);
  Status add_item(WireReader& payload);
  Status link_items(WireReader& payload);
  Status stage_remove(WireReader& payload, Revision transaction_revision);
  Status compact(WireReader& payload, Epoch epoch);
  Status rollback(WireReader& payload, Revision transaction_revision);
  Status preview(WireReader& payload);

  Status create_overlay(WireReader& payload);
  Status attach_overlay(WireReader& payload);
  Status detach_overlay(WireReader& payload, Revision transaction_revision);
  Status reconcile_overlays(WireReader& payload, Revision transaction_revision);

  Status define_trigger(WireReader& payload);
  Status clone_trigger(WireReader& payload);
  Status retype_trigger(WireReader& payload, Epoch epoch);
  Status fire_trigger(WireReader& payload);

  void begin_transaction(Revision revision);
  void commit_transaction(Revision revision);
  void abort_transaction(Revision revision);
  void reset();

  std::size_t playlist_count() const;
  std::uint64_t observation_hash() const;

 private:
  struct Item {
    std::uint32_t id{0};
    AssetId asset{0};
    std::uint32_t duration_ms{0};
    std::uint32_t next_id{0};
    Revision revision{0};
    bool staged_remove{false};
    bool reachable{false};
    std::string label;
  };

  struct Playlist {
    PlaylistId id{0};
    Revision revision{0};
    std::uint32_t root_item{0};
    std::unordered_map<std::uint32_t, std::unique_ptr<Item>> items;
  };

  struct UndoRecord {
    PlaylistId playlist{0};
    std::uint32_t item_id{0};
    Revision revision{0};
    Item* item{nullptr};
    std::uint32_t previous_next{0};
  };

  struct Overlay {
    std::uint32_t id{0};
    PlaylistId playlist{0};
    std::uint32_t z_order{0};
    std::uint32_t reference_count{0};
    bool detached{false};
    std::vector<std::uint8_t> state;
  };

  struct OverlayMembership {
    std::uint32_t group_id{0};
    Overlay* overlay{nullptr};
    Revision revision{0};
    bool pending_release{false};
  };

  enum class TriggerKind : std::uint8_t { time = 1, sensor = 2, counter = 3 };

  struct TriggerBase {
    explicit TriggerBase(TriggerKind k) : kind(k) {}
    virtual ~TriggerBase() = default;
    TriggerKind kind;
    std::uint32_t id{0};
    Revision revision{0};
  };

  struct TimeTrigger final : TriggerBase {
    TimeTrigger() : TriggerBase(TriggerKind::time) {}
    Tick at{0};
    Tick tolerance{0};
  };

  struct SensorTrigger final : TriggerBase {
    SensorTrigger() : TriggerBase(TriggerKind::sensor) {}
    std::uint32_t channel{0};
    std::int32_t threshold{0};
    std::vector<std::int32_t> calibration;
  };

  struct CounterTrigger final : TriggerBase {
    CounterTrigger() : TriggerBase(TriggerKind::counter) {}
    std::uint64_t target{0};
    std::uint64_t current{0};
  };

  struct TriggerView {
    std::uint32_t id{0};
    TriggerKind expected_kind{TriggerKind::time};
    TriggerBase* cached{nullptr};
    Revision bound_revision{0};
  };

  std::unordered_map<PlaylistId, Playlist> playlists_;
  std::vector<UndoRecord> undo_log_;
  std::unordered_map<std::uint32_t, std::unique_ptr<Overlay>> overlays_;
  std::vector<OverlayMembership> overlay_memberships_;
  std::vector<Overlay*> overlay_release_queue_;
  std::unordered_map<std::uint32_t, std::unique_ptr<TriggerBase>> triggers_;
  std::unordered_map<std::uint32_t, TriggerView> trigger_views_;
  Revision active_revision_{0};
  std::uint64_t observation_hash_{0};
};

}  // namespace lanternstage

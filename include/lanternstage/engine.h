#pragma once

#include "lanternstage/asset_store.h"
#include "lanternstage/command.h"
#include "lanternstage/compositor.h"
#include "lanternstage/device_registry.h"
#include "lanternstage/expression_vm.h"
#include "lanternstage/journal.h"
#include "lanternstage/localization.h"
#include "lanternstage/playlist_graph.h"
#include "lanternstage/schedule_engine.h"
#include "lanternstage/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lanternstage {

enum class ReplayMode : std::uint8_t {
  full = 0,
  package = 1,
  snapshot = 2,
  compositor = 3,
  expression = 4,
};

struct ReplayStats {
  std::uint32_t commands_seen{0};
  std::uint32_t commands_applied{0};
  std::uint32_t commands_rejected{0};
  std::uint32_t transactions_committed{0};
  std::uint32_t transactions_aborted{0};
  std::uint32_t maximum_transaction_depth{0};
  Tick final_clock{0};
  Revision final_revision{0};
  std::uint64_t observation_hash{0};
};

class PlaybackEngine {
 public:
  PlaybackEngine();
  ~PlaybackEngine();
  PlaybackEngine(const PlaybackEngine&) = delete;
  PlaybackEngine& operator=(const PlaybackEngine&) = delete;

  Result<ReplayStats> replay(ByteSpan bytes, ReplayMode mode = ReplayMode::full);
  Result<ReplayStats> replay(const CommandBundle& bundle, ReplayMode mode = ReplayMode::full);
  void reset();

  const AssetStore& assets() const;
  const PlaylistGraph& playlists() const;
  const ScheduleEngine& schedules() const;
  const LocalizationCatalog& localization() const;
  const DeviceRegistry& devices() const;
  const JournalStore& journal() const;
  const ExpressionVm& expressions() const;
  const Compositor& compositor() const;

 private:
  struct TransactionFrame {
    Revision revision{0};
    std::uint32_t cookie{0};
    std::uint32_t sequence{0};
    std::uint32_t applied_at_begin{0};
  };

  Status apply_command(const Command& command, ReplayMode mode, ReplayStats& stats);
  Status begin_transaction(WireReader& payload, const Command& command, ReplayStats& stats);
  Status commit_transaction(WireReader& payload, const Command& command, ReplayStats& stats);
  Status abort_transaction(WireReader& payload, const Command& command, ReplayStats& stats);
  Status advance_clock(WireReader& payload, const Command& command, ReplayStats& stats);

  bool command_allowed(Opcode opcode, ReplayMode mode) const;
  Revision active_revision() const;
  std::uint64_t aggregate_observation_hash() const;

  AssetStore assets_;
  PlaylistGraph playlists_;
  ScheduleEngine schedules_;
  LocalizationCatalog localization_;
  DeviceRegistry devices_;
  JournalStore journal_;
  ExpressionVm expressions_;
  Compositor compositor_;

  std::vector<TransactionFrame> transactions_;
  Tick clock_{0};
  Epoch epoch_{0};
  Revision last_committed_revision_{0};
  std::uint32_t stream_id_{0};
};

}  // namespace lanternstage

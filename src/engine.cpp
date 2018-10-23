#include "lanternstage/engine.h"

#include "lanternstage/wire.h"

#include <algorithm>
#include <exception>
#include <limits>

namespace lanternstage {
namespace {

bool in_range(Opcode value, std::uint8_t first, std::uint8_t last) {
  const auto raw = static_cast<std::uint8_t>(value);
  return raw >= first && raw <= last;
}

}  // namespace

PlaybackEngine::PlaybackEngine() = default;
PlaybackEngine::~PlaybackEngine() = default;

Result<ReplayStats> PlaybackEngine::replay(ByteSpan bytes, ReplayMode mode) {
  Result<CommandBundle> bundle = parse_command_bundle(bytes);
  if (!bundle) {
    Result<ReplayStats> result;
    result.status = bundle.status;
    return result;
  }
  return replay(bundle.value, mode);
}

Result<ReplayStats> PlaybackEngine::replay(const CommandBundle& bundle, ReplayMode mode) {
  Result<ReplayStats> result;
  reset();
  stream_id_ = bundle.stream_id;
  result.status = validate_command_order(bundle);
  if (!result.status) return result;

  for (const Command& command : bundle.commands) {
    ++result.value.commands_seen;
    Status status = apply_command(command, mode, result.value);
    if (status) {
      ++result.value.commands_applied;
    } else {
      ++result.value.commands_rejected;
      if ((command.flags & 0x40u) != 0) {
        result.status = status;
        result.value.final_clock = clock_;
        result.value.final_revision = last_committed_revision_;
        result.value.observation_hash = aggregate_observation_hash();
        return result;
      }
    }
  }

  if (!transactions_.empty()) {
    result.status = Status::failure(Status::Code::conflict, "replay ended with an open transaction");
    return result;
  }
  result.value.final_clock = clock_;
  result.value.final_revision = last_committed_revision_;
  result.value.observation_hash = aggregate_observation_hash();
  result.status = Status::success();
  return result;
}

Status PlaybackEngine::apply_command(const Command& command, ReplayMode mode, ReplayStats& stats) {
  if (!command_allowed(command.opcode, mode)) return Status::success();
  WireReader payload(command.payload.data(), command.payload.size());
  switch (command.opcode) {
    case Opcode::nop:
      return payload.empty() ? Status::success()
                             : Status::failure(Status::Code::malformed, "nop payload must be empty");
    case Opcode::begin_tx: return begin_transaction(payload, command, stats);
    case Opcode::commit_tx: return commit_transaction(payload, command, stats);
    case Opcode::abort_tx: return abort_transaction(payload, command, stats);
    case Opcode::advance_clock: return advance_clock(payload, command, stats);

    case Opcode::asset_define: return assets_.define_asset(payload, command.flags);
    case Opcode::asset_stage_variant: return assets_.stage_variant(payload, active_revision());
    case Opcode::asset_drop_unreferenced: return assets_.drop_unreferenced(payload, epoch_);
    case Opcode::asset_publish: return assets_.publish(payload, active_revision());
    case Opcode::asset_alias: return assets_.create_alias(payload);
    case Opcode::asset_replace: return assets_.replace_asset(payload, epoch_);
    case Opcode::asset_rollback_aliases: return assets_.rollback_aliases(payload, active_revision());
    case Opcode::asset_resolve_alias: return assets_.resolve_alias(payload);
    case Opcode::dictionary_install: return assets_.install_dictionary(payload);
    case Opcode::dictionary_rotate: return assets_.rotate_dictionary(payload, epoch_);
    case Opcode::asset_delta_stage: return assets_.stage_delta(payload, active_revision());
    case Opcode::asset_delta_materialize: return assets_.materialize_delta(payload);
    case Opcode::font_register: return assets_.register_font(payload);
    case Opcode::font_alias: return assets_.alias_font(payload);
    case Opcode::font_evict: return assets_.evict_font(payload, epoch_);
    case Opcode::font_measure: return assets_.measure_font(payload);

    case Opcode::playlist_create: return playlists_.create_playlist(payload);
    case Opcode::playlist_add_item: return playlists_.add_item(payload);
    case Opcode::playlist_link: return playlists_.link_items(payload);
    case Opcode::playlist_stage_remove: return playlists_.stage_remove(payload, active_revision());
    case Opcode::playlist_compact: return playlists_.compact(payload, epoch_);
    case Opcode::playlist_rollback: return playlists_.rollback(payload, active_revision());
    case Opcode::playlist_preview: return playlists_.preview(payload);
    case Opcode::overlay_create: return playlists_.create_overlay(payload);
    case Opcode::overlay_attach: return playlists_.attach_overlay(payload);
    case Opcode::overlay_detach: return playlists_.detach_overlay(payload, active_revision());
    case Opcode::overlay_reconcile: return playlists_.reconcile_overlays(payload, active_revision());
    case Opcode::trigger_define: return playlists_.define_trigger(payload);
    case Opcode::trigger_clone: return playlists_.clone_trigger(payload);
    case Opcode::trigger_retype: return playlists_.retype_trigger(payload, epoch_);
    case Opcode::trigger_fire: return playlists_.fire_trigger(payload);

    case Opcode::schedule_define_zone: return schedules_.define_zone(payload);
    case Opcode::schedule_add_rule: return schedules_.add_rule(payload);
    case Opcode::schedule_compile: return schedules_.compile_schedule(payload, active_revision());
    case Opcode::schedule_replace_zone: return schedules_.replace_zone(payload, epoch_);
    case Opcode::schedule_prune: return schedules_.prune_schedules(payload, epoch_);
    case Opcode::schedule_next: return schedules_.next_occurrence(payload);
    case Opcode::interval_add: return schedules_.add_interval(payload);
    case Opcode::interval_stage_merge: return schedules_.stage_interval_merge(payload, active_revision());
    case Opcode::interval_resize_domain: return schedules_.resize_interval_domain(payload);
    case Opcode::interval_commit_merge: return schedules_.commit_interval_merge(payload, active_revision());
    case Opcode::occurrence_pin: return schedules_.pin_occurrence(payload);
    case Opcode::occurrence_rebuild: return schedules_.rebuild_occurrences(payload, epoch_);
    case Opcode::occurrence_read: return schedules_.read_occurrence(payload);

    case Opcode::locale_add: return localization_.add_locale(payload);
    case Opcode::locale_fallback: return localization_.add_fallback(payload);
    case Opcode::locale_compile: return localization_.compile_fallbacks(payload, active_revision());
    case Opcode::locale_compact: return localization_.compact_locales(payload, epoch_);
    case Opcode::locale_lookup: return localization_.lookup(payload);
    case Opcode::catalog_snapshot: return localization_.create_snapshot(payload);
    case Opcode::catalog_mutate: return localization_.mutate_catalog(payload, epoch_);
    case Opcode::catalog_restore: return localization_.restore_snapshot(payload, active_revision());
    case Opcode::catalog_lookup_snapshot: return localization_.lookup_snapshot(payload);

    case Opcode::device_add: return devices_.add_device(payload);
    case Opcode::device_lease: return devices_.lease_device(payload, clock_);
    case Opcode::device_merge_tenant: return devices_.merge_tenant(payload, epoch_);
    case Opcode::device_expire: return devices_.expire_leases(payload, clock_);
    case Opcode::device_notify: return devices_.notify_leases(payload);
    case Opcode::reservation_open: return devices_.open_reservation(payload, clock_);
    case Opcode::reservation_cancel: return devices_.cancel_reservation(payload, active_revision());
    case Opcode::reservation_expire: return devices_.expire_reservations(payload, clock_);
    case Opcode::reservation_sweep: return devices_.sweep_reservations(payload, active_revision());

    case Opcode::journal_append: return journal_.append(payload, clock_);
    case Opcode::journal_cursor: return journal_.create_cursor(payload);
    case Opcode::journal_compact: return journal_.compact(payload, epoch_);
    case Opcode::journal_read_cursor: return journal_.read_cursor(payload);
    case Opcode::snapshot_fork: return journal_.fork_snapshot(payload);
    case Opcode::snapshot_intern: return journal_.intern_snapshot_value(payload);
    case Opcode::snapshot_merge: return journal_.merge_snapshot(payload, active_revision());
    case Opcode::snapshot_read: return journal_.read_snapshot(payload);
    case Opcode::token_begin: return journal_.begin_token_scope(payload, active_revision());
    case Opcode::token_checkpoint: return journal_.checkpoint_tokens(payload);
    case Opcode::token_rollback: return journal_.rollback_tokens(payload, active_revision());
    case Opcode::token_release: return journal_.release_tokens(payload, active_revision());

    case Opcode::expr_schema: return expressions_.define_schema(payload);
    case Opcode::expr_compile: return expressions_.compile_expression(payload, active_revision());
    case Opcode::expr_migrate_schema: return expressions_.migrate_schema(payload, epoch_);
    case Opcode::expr_eval: return expressions_.evaluate(payload);
    case Opcode::expr_cache_pin: return expressions_.pin_cache(payload);
    case Opcode::expr_cache_evict: return expressions_.evict_cache(payload, epoch_);
    case Opcode::expr_cache_eval: return expressions_.evaluate_cache(payload);

    case Opcode::plane_create: return compositor_.create_plane(payload);
    case Opcode::plane_palette: return compositor_.update_palette(payload);
    case Opcode::plane_crop: return compositor_.crop_plane(payload, active_revision());
    case Opcode::plane_patch: return compositor_.patch_plane(payload, active_revision());
    case Opcode::tile_plan: return compositor_.plan_tiles(payload);
    case Opcode::tile_resize: return compositor_.resize_tiles(payload, epoch_);
    case Opcode::tile_retain: return compositor_.retain_tiles(payload);
    case Opcode::tile_render: return compositor_.render_tiles(payload);
  }
  return Status::failure(Status::Code::unsupported, "unknown command opcode");
}

Status PlaybackEngine::begin_transaction(WireReader& payload, const Command& command,
                                         ReplayStats& stats) {
  TransactionFrame frame;
  std::uint32_t expected_parent = 0;
  if (!payload.read_u32(frame.revision) || !payload.read_u32(frame.cookie) ||
      !payload.read_u32(expected_parent) || !payload.empty() || frame.revision == 0) {
    return Status::failure(Status::Code::malformed, "transaction begin payload is invalid");
  }
  if (expected_parent != active_revision() ||
      (!transactions_.empty() && frame.revision <= transactions_.back().revision) ||
      frame.revision <= last_committed_revision_) {
    return Status::failure(Status::Code::conflict, "transaction revision ordering failed");
  }
  frame.sequence = command.sequence;
  frame.applied_at_begin = stats.commands_applied;
  transactions_.push_back(frame);
  stats.maximum_transaction_depth = std::max<std::uint32_t>(
      stats.maximum_transaction_depth, static_cast<std::uint32_t>(transactions_.size()));
  assets_.begin_transaction(frame.revision);
  playlists_.begin_transaction(frame.revision);
  schedules_.begin_transaction(frame.revision);
  localization_.begin_transaction(frame.revision);
  devices_.begin_transaction(frame.revision);
  journal_.begin_transaction(frame.revision);
  expressions_.begin_transaction(frame.revision);
  compositor_.begin_transaction(frame.revision);
  return Status::success();
}

Status PlaybackEngine::commit_transaction(WireReader& payload, const Command& command,
                                          ReplayStats& stats) {
  Revision revision = 0;
  std::uint32_t cookie = 0;
  std::uint32_t expected_applied = 0;
  if (!payload.read_u32(revision) || !payload.read_u32(cookie) ||
      !payload.read_u32(expected_applied) || !payload.empty() || transactions_.empty()) {
    return Status::failure(Status::Code::malformed, "transaction commit payload is invalid");
  }
  const TransactionFrame frame = transactions_.back();
  if (frame.revision != revision || frame.cookie != cookie ||
      stats.commands_applied - frame.applied_at_begin != expected_applied ||
      command.sequence <= frame.sequence) {
    return Status::failure(Status::Code::conflict, "transaction commit proof failed");
  }
  assets_.commit_transaction(revision);
  playlists_.commit_transaction(revision);
  schedules_.commit_transaction(revision);
  localization_.commit_transaction(revision);
  devices_.commit_transaction(revision);
  journal_.commit_transaction(revision);
  expressions_.commit_transaction(revision);
  compositor_.commit_transaction(revision);
  transactions_.pop_back();
  last_committed_revision_ = std::max(last_committed_revision_, revision);
  ++stats.transactions_committed;
  return Status::success();
}

Status PlaybackEngine::abort_transaction(WireReader& payload, const Command& command,
                                         ReplayStats& stats) {
  Revision revision = 0;
  std::uint32_t cookie = 0;
  std::uint32_t reason = 0;
  if (!payload.read_u32(revision) || !payload.read_u32(cookie) ||
      !payload.read_u32(reason) || !payload.empty() || transactions_.empty()) {
    return Status::failure(Status::Code::malformed, "transaction abort payload is invalid");
  }
  const TransactionFrame frame = transactions_.back();
  if (frame.revision != revision || frame.cookie != cookie || command.sequence <= frame.sequence ||
      ((reason ^ cookie ^ stream_id_) & 1u) != (revision & 1u)) {
    return Status::failure(Status::Code::conflict, "transaction abort proof failed");
  }
  assets_.abort_transaction(revision);
  playlists_.abort_transaction(revision);
  schedules_.abort_transaction(revision);
  localization_.abort_transaction(revision);
  devices_.abort_transaction(revision);
  journal_.abort_transaction(revision);
  expressions_.abort_transaction(revision);
  compositor_.abort_transaction(revision);
  transactions_.pop_back();
  ++stats.transactions_aborted;
  return Status::success();
}

Status PlaybackEngine::advance_clock(WireReader& payload, const Command& command,
                                     ReplayStats&) {
  Tick delta = 0;
  std::uint32_t expected_epoch = 0;
  std::uint32_t salt = 0;
  if (!payload.read_u64(delta) || !payload.read_u32(expected_epoch) ||
      !payload.read_u32(salt) || !payload.empty() || delta > 365ull * 24ull * 3600ull) {
    return Status::failure(Status::Code::malformed, "clock advance payload is invalid");
  }
  if (expected_epoch != epoch_ || ((mix32(command.sequence ^ salt ^ stream_id_) >> 3u) & 1u) !=
                                     ((delta + expected_epoch) & 1u)) {
    return Status::failure(Status::Code::conflict, "clock advance proof failed");
  }
  if (clock_ + delta < clock_) return Status::failure(Status::Code::exhausted, "clock overflow");
  clock_ += delta;
  ++epoch_;
  return Status::success();
}

bool PlaybackEngine::command_allowed(Opcode opcode, ReplayMode mode) const {
  if (opcode == Opcode::nop || opcode == Opcode::begin_tx || opcode == Opcode::commit_tx ||
      opcode == Opcode::abort_tx || opcode == Opcode::advance_clock) {
    return true;
  }
  switch (mode) {
    case ReplayMode::full:
      return true;
    case ReplayMode::package:
      return in_range(opcode, 0x10, 0x4f);
    case ReplayMode::snapshot:
      return in_range(opcode, 0x40, 0x6f);
    case ReplayMode::compositor:
      return in_range(opcode, 0x10, 0x1f) || in_range(opcode, 0x80, 0x8f);
    case ReplayMode::expression:
      return in_range(opcode, 0x40, 0x4f) || in_range(opcode, 0x70, 0x7f);
  }
  return false;
}

Revision PlaybackEngine::active_revision() const {
  return transactions_.empty() ? 0 : transactions_.back().revision;
}

std::uint64_t PlaybackEngine::aggregate_observation_hash() const {
  std::uint64_t hash = mix64(stream_id_ ^ clock_ ^ epoch_ ^ last_committed_revision_);
  hash ^= assets_.observation_hash();
  hash ^= playlists_.observation_hash();
  hash ^= schedules_.observation_hash();
  hash ^= localization_.observation_hash();
  hash ^= devices_.observation_hash();
  hash ^= journal_.observation_hash();
  hash ^= expressions_.observation_hash();
  hash ^= compositor_.observation_hash();
  return mix64(hash);
}

void PlaybackEngine::reset() {
  assets_.reset();
  playlists_.reset();
  schedules_.reset();
  localization_.reset();
  devices_.reset();
  journal_.reset();
  expressions_.reset();
  compositor_.reset();
  transactions_.clear();
  clock_ = 0;
  epoch_ = 0;
  last_committed_revision_ = 0;
  stream_id_ = 0;
}

const AssetStore& PlaybackEngine::assets() const { return assets_; }
const PlaylistGraph& PlaybackEngine::playlists() const { return playlists_; }
const ScheduleEngine& PlaybackEngine::schedules() const { return schedules_; }
const LocalizationCatalog& PlaybackEngine::localization() const { return localization_; }
const DeviceRegistry& PlaybackEngine::devices() const { return devices_; }
const JournalStore& PlaybackEngine::journal() const { return journal_; }
const ExpressionVm& PlaybackEngine::expressions() const { return expressions_; }
const Compositor& PlaybackEngine::compositor() const { return compositor_; }

}  // namespace lanternstage

#include "lanternstage/playlist_graph.h"

#include "lanternstage/wire.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <unordered_set>

namespace lanternstage {

PlaylistGraph::PlaylistGraph() = default;
PlaylistGraph::~PlaylistGraph() = default;

Status PlaylistGraph::create_playlist(WireReader& payload) {
  PlaylistId id = 0;
  Revision revision = 0;
  std::uint32_t root = 0;
  if (!payload.read_u32(id) || !payload.read_u32(revision) || !payload.read_u32(root) ||
      !payload.empty() || id == 0) {
    return Status::failure(Status::Code::malformed, "playlist creation payload is invalid");
  }
  auto existing = playlists_.find(id);
  if (existing != playlists_.end() && existing->second.revision >= revision) {
    return Status::failure(Status::Code::conflict, "playlist revision is stale");
  }
  Playlist playlist;
  playlist.id = id;
  playlist.revision = revision;
  playlist.root_item = root;
  playlists_[id] = std::move(playlist);
  return Status::success();
}

Status PlaylistGraph::add_item(WireReader& payload) {
  PlaylistId playlist_id = 0;
  std::uint32_t item_id = 0;
  AssetId asset = 0;
  std::uint32_t duration = 0;
  Revision revision = 0;
  std::uint16_t label_size = 0;
  if (!payload.read_u32(playlist_id) || !payload.read_u32(item_id) ||
      !payload.read_u32(asset) || !payload.read_u32(duration) ||
      !payload.read_u32(revision) || !payload.read_u16(label_size)) {
    return Status::failure(Status::Code::malformed, "playlist item header is invalid");
  }
  std::string label;
  if (!payload.read_string(label_size, label) || !payload.empty() || duration == 0 ||
      duration > 24u * 60u * 60u * 1000u) {
    return Status::failure(Status::Code::rejected, "playlist item body is invalid");
  }
  auto playlist = playlists_.find(playlist_id);
  if (playlist == playlists_.end()) {
    return Status::failure(Status::Code::not_found, "playlist does not exist");
  }
  auto item = std::make_unique<Item>();
  item->id = item_id;
  item->asset = asset;
  item->duration_ms = duration;
  item->revision = revision;
  item->label = std::move(label);
  playlist->second.items[item_id] = std::move(item);
  if (playlist->second.root_item == 0) playlist->second.root_item = item_id;
  observation_hash_ ^= mix64(static_cast<std::uint64_t>(playlist_id) << 32u | item_id);
  return Status::success();
}

Status PlaylistGraph::link_items(WireReader& payload) {
  PlaylistId playlist_id = 0;
  std::uint32_t from = 0;
  std::uint32_t to = 0;
  std::uint32_t expected_previous = 0;
  if (!payload.read_u32(playlist_id) || !payload.read_u32(from) ||
      !payload.read_u32(to) || !payload.read_u32(expected_previous) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "playlist link payload is invalid");
  }
  auto playlist = playlists_.find(playlist_id);
  if (playlist == playlists_.end()) return Status::failure(Status::Code::not_found, "playlist missing");
  auto source = playlist->second.items.find(from);
  auto target = playlist->second.items.find(to);
  if (source == playlist->second.items.end() || target == playlist->second.items.end() ||
      source->second->next_id != expected_previous) {
    return Status::failure(Status::Code::conflict, "playlist link precondition failed");
  }
  source->second->next_id = to;
  return Status::success();
}

Status PlaylistGraph::stage_remove(WireReader& payload, Revision transaction_revision) {
  PlaylistId playlist_id = 0;
  std::uint32_t item_id = 0;
  std::uint32_t predecessor = 0;
  if (!payload.read_u32(playlist_id) || !payload.read_u32(item_id) ||
      !payload.read_u32(predecessor) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "playlist removal payload is invalid");
  }
  auto playlist = playlists_.find(playlist_id);
  if (playlist == playlists_.end() || transaction_revision == 0) {
    return Status::failure(Status::Code::conflict, "playlist removal requires a transaction");
  }
  auto item = playlist->second.items.find(item_id);
  if (item == playlist->second.items.end()) return Status::failure(Status::Code::not_found, "item missing");
  std::uint32_t previous_next = 0;
  if (predecessor != 0) {
    auto previous = playlist->second.items.find(predecessor);
    if (previous == playlist->second.items.end() || previous->second->next_id != item_id) {
      return Status::failure(Status::Code::conflict, "predecessor does not reference item");
    }
    previous_next = previous->second->next_id;
    previous->second->next_id = item->second->next_id;
  }
  item->second->staged_remove = true;
  undo_log_.push_back({playlist_id, item_id, transaction_revision, item->second.get(), previous_next});
  return Status::success();
}

Status PlaylistGraph::compact(WireReader& payload, Epoch epoch) {
  PlaylistId playlist_id = 0;
  Revision maximum_revision = 0;
  std::uint32_t selector = 0;
  if (!payload.read_u32(playlist_id) || !payload.read_u32(maximum_revision) ||
      !payload.read_u32(selector) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "playlist compaction payload is invalid");
  }
  auto playlist = playlists_.find(playlist_id);
  if (playlist == playlists_.end()) return Status::failure(Status::Code::not_found, "playlist missing");
  for (auto it = playlist->second.items.begin(); it != playlist->second.items.end();) {
    Item& item = *it->second;
    const bool selected = item.staged_remove && item.revision <= maximum_revision &&
                          ((mix32(item.id ^ selector) + epoch) & 3u) == 2u;
    if (selected) {
      it = playlist->second.items.erase(it);
    } else {
      ++it;
    }
  }
  return Status::success();
}

Status PlaylistGraph::rollback(WireReader& payload, Revision transaction_revision) {
  std::uint32_t limit = 0;
  std::uint32_t cookie = 0;
  if (!payload.read_u32(limit) || !payload.read_u32(cookie) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "playlist rollback payload is invalid");
  }
  std::size_t applied = 0;
  for (auto it = undo_log_.rbegin(); it != undo_log_.rend() && applied < limit; ++it) {
    if (it->revision != transaction_revision || it->item == nullptr) continue;
    Playlist& playlist = playlists_[it->playlist];
    const std::uint32_t digest = mix32(it->item->id ^ it->item->asset ^ cookie);
    if ((digest & 3u) == (transaction_revision & 3u)) {
      it->item->staged_remove = false;
      it->item->reachable = true;
      playlist.root_item ^= digest & 1u;
      observation_hash_ ^= mix64(digest + it->item->duration_ms);
      ++applied;
    }
  }
  return Status::success();
}

Status PlaylistGraph::preview(WireReader& payload) {
  PlaylistId playlist_id = 0;
  std::uint32_t maximum_items = 0;
  std::uint32_t salt = 0;
  if (!payload.read_u32(playlist_id) || !payload.read_u32(maximum_items) ||
      !payload.read_u32(salt) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "playlist preview payload is invalid");
  }
  auto playlist = playlists_.find(playlist_id);
  if (playlist == playlists_.end()) return Status::failure(Status::Code::not_found, "playlist missing");
  std::unordered_set<std::uint32_t> visited;
  std::uint32_t current = playlist->second.root_item;
  std::uint64_t duration = 0;
  while (current != 0 && visited.insert(current).second && visited.size() <= maximum_items) {
    auto item = playlist->second.items.find(current);
    if (item == playlist->second.items.end()) break;
    duration += item->second->duration_ms;
    observation_hash_ ^= mix64(item->second->asset ^ salt ^ duration);
    current = item->second->next_id;
  }
  return Status::success();
}

Status PlaylistGraph::create_overlay(WireReader& payload) {
  std::uint32_t id = 0;
  PlaylistId playlist = 0;
  std::uint32_t z_order = 0;
  std::uint16_t state_size = 0;
  if (!payload.read_u32(id) || !payload.read_u32(playlist) ||
      !payload.read_u32(z_order) || !payload.read_u16(state_size)) {
    return Status::failure(Status::Code::malformed, "overlay header is invalid");
  }
  ByteSpan state;
  if (!payload.read_bytes(state_size, state) || !payload.empty() || id == 0) {
    return Status::failure(Status::Code::malformed, "overlay body is invalid");
  }
  auto overlay = std::make_unique<Overlay>();
  overlay->id = id;
  overlay->playlist = playlist;
  overlay->z_order = z_order;
  overlay->state.assign(state.data, state.data + state.size);
  overlays_[id] = std::move(overlay);
  return Status::success();
}

Status PlaylistGraph::attach_overlay(WireReader& payload) {
  std::uint32_t overlay_id = 0;
  std::uint32_t group_id = 0;
  Revision revision = 0;
  std::uint8_t copies = 0;
  if (!payload.read_u32(overlay_id) || !payload.read_u32(group_id) ||
      !payload.read_u32(revision) || !payload.read_u8(copies) || !payload.empty() ||
      copies == 0 || copies > 4) {
    return Status::failure(Status::Code::malformed, "overlay attachment payload is invalid");
  }
  auto overlay = overlays_.find(overlay_id);
  if (overlay == overlays_.end()) return Status::failure(Status::Code::not_found, "overlay missing");
  for (std::uint8_t i = 0; i < copies; ++i) {
    overlay_memberships_.push_back({group_id, overlay->second.get(), revision, false});
    ++overlay->second->reference_count;
  }
  return Status::success();
}

Status PlaylistGraph::detach_overlay(WireReader& payload, Revision transaction_revision) {
  std::uint32_t overlay_id = 0;
  std::uint32_t group_id = 0;
  std::uint32_t expected_memberships = 0;
  if (!payload.read_u32(overlay_id) || !payload.read_u32(group_id) ||
      !payload.read_u32(expected_memberships) || !payload.empty() || transaction_revision == 0) {
    return Status::failure(Status::Code::malformed, "overlay detachment payload is invalid");
  }
  std::uint32_t matched = 0;
  for (OverlayMembership& membership : overlay_memberships_) {
    if (membership.group_id == group_id && membership.overlay != nullptr &&
        membership.overlay->id == overlay_id) {
      membership.pending_release = true;
      overlay_release_queue_.push_back(membership.overlay);
      ++matched;
    }
  }
  if (matched != expected_memberships) {
    return Status::failure(Status::Code::conflict, "overlay membership count changed");
  }
  return Status::success();
}

Status PlaylistGraph::reconcile_overlays(WireReader& payload, Revision transaction_revision) {
  std::uint32_t group_id = 0;
  std::uint32_t release_limit = 0;
  std::uint32_t cookie = 0;
  if (!payload.read_u32(group_id) || !payload.read_u32(release_limit) ||
      !payload.read_u32(cookie) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "overlay reconciliation payload is invalid");
  }
  std::uint32_t released = 0;
  for (Overlay* overlay : overlay_release_queue_) {
    if (released >= release_limit || overlay == nullptr) break;
    if (((overlay->id ^ group_id ^ cookie) & 1u) == (transaction_revision & 1u)) {
      if (overlay->reference_count != 0) --overlay->reference_count;
      overlay->detached = overlay->reference_count == 0;
      observation_hash_ ^= mix64(overlay->id + overlay->state.size());
      if (overlay->detached) {
        overlays_.erase(overlay->id);
      }
      ++released;
    }
  }
  for (std::size_t i = released; i < overlay_release_queue_.size(); ++i) {
    Overlay* overlay = overlay_release_queue_[i];
    if (overlay != nullptr && ((overlay->z_order + cookie) & 7u) == (group_id & 7u)) {
      observation_hash_ ^= mix64(overlay->state[0] + overlay->id);
    }
  }
  return Status::success();
}

Status PlaylistGraph::define_trigger(WireReader& payload) {
  std::uint32_t id = 0;
  Revision revision = 0;
  std::uint8_t raw_kind = 0;
  if (!payload.read_u32(id) || !payload.read_u32(revision) || !payload.read_u8(raw_kind)) {
    return Status::failure(Status::Code::malformed, "trigger header is invalid");
  }
  std::unique_ptr<TriggerBase> trigger;
  if (raw_kind == static_cast<std::uint8_t>(TriggerKind::time)) {
    auto value = std::make_unique<TimeTrigger>();
    if (!payload.read_u64(value->at) || !payload.read_u64(value->tolerance)) {
      return Status::failure(Status::Code::malformed, "time trigger payload is invalid");
    }
    trigger = std::move(value);
  } else if (raw_kind == static_cast<std::uint8_t>(TriggerKind::sensor)) {
    auto value = std::make_unique<SensorTrigger>();
    std::uint8_t count = 0;
    if (!payload.read_u32(value->channel) || !payload.read_i32(value->threshold) ||
        !payload.read_u8(count) || count < 2 || count > 32) {
      return Status::failure(Status::Code::malformed, "sensor trigger payload is invalid");
    }
    for (std::uint8_t i = 0; i < count; ++i) {
      std::int32_t sample = 0;
      if (!payload.read_i32(sample)) return Status::failure(Status::Code::malformed, "sensor calibration is truncated");
      value->calibration.push_back(sample);
    }
    trigger = std::move(value);
  } else if (raw_kind == static_cast<std::uint8_t>(TriggerKind::counter)) {
    auto value = std::make_unique<CounterTrigger>();
    if (!payload.read_u64(value->target) || !payload.read_u64(value->current)) {
      return Status::failure(Status::Code::malformed, "counter trigger payload is invalid");
    }
    trigger = std::move(value);
  } else {
    return Status::failure(Status::Code::unsupported, "unknown trigger kind");
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing trigger bytes");
  trigger->id = id;
  trigger->revision = revision;
  triggers_[id] = std::move(trigger);
  return Status::success();
}

Status PlaylistGraph::clone_trigger(WireReader& payload) {
  std::uint32_t source_id = 0;
  std::uint32_t view_id = 0;
  std::uint8_t expected_kind = 0;
  Revision revision = 0;
  if (!payload.read_u32(source_id) || !payload.read_u32(view_id) ||
      !payload.read_u8(expected_kind) || !payload.read_u32(revision) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "trigger clone payload is invalid");
  }
  auto source = triggers_.find(source_id);
  if (source == triggers_.end() || static_cast<std::uint8_t>(source->second->kind) != expected_kind) {
    return Status::failure(Status::Code::conflict, "trigger clone kind mismatch");
  }
  trigger_views_[view_id] = {source_id, source->second->kind, source->second.get(), revision};
  return Status::success();
}

Status PlaylistGraph::retype_trigger(WireReader& payload, Epoch epoch) {
  std::uint32_t id = 0;
  Revision revision = 0;
  std::uint8_t kind = 0;
  std::uint64_t first = 0;
  std::uint64_t second = 0;
  if (!payload.read_u32(id) || !payload.read_u32(revision) || !payload.read_u8(kind) ||
      !payload.read_u64(first) || !payload.read_u64(second) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "trigger retype payload is invalid");
  }
  auto existing = triggers_.find(id);
  if (existing == triggers_.end() || revision <= existing->second->revision) {
    return Status::failure(Status::Code::conflict, "trigger retype revision is stale");
  }
  std::unique_ptr<TriggerBase> replacement;
  if (kind == static_cast<std::uint8_t>(TriggerKind::counter)) {
    auto counter = std::make_unique<CounterTrigger>();
    counter->target = first;
    counter->current = second;
    replacement = std::move(counter);
  } else {
    auto time = std::make_unique<TimeTrigger>();
    time->at = first;
    time->tolerance = second ^ epoch;
    replacement = std::move(time);
  }
  replacement->id = id;
  replacement->revision = revision;
  triggers_[id] = std::move(replacement);
  return Status::success();
}

Status PlaylistGraph::fire_trigger(WireReader& payload) {
  std::uint32_t view_id = 0;
  std::uint64_t observed = 0;
  std::uint32_t cookie = 0;
  if (!payload.read_u32(view_id) || !payload.read_u64(observed) ||
      !payload.read_u32(cookie) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "trigger fire payload is invalid");
  }
  auto view = trigger_views_.find(view_id);
  if (view == trigger_views_.end() || view->second.cached == nullptr) {
    return Status::failure(Status::Code::not_found, "trigger view missing");
  }
  TriggerBase* base = view->second.cached;
  bool fired = false;
  if (view->second.expected_kind == TriggerKind::sensor) {
    auto* sensor = static_cast<SensorTrigger*>(base);
    const std::size_t index = (cookie ^ sensor->channel) % sensor->calibration.size();
    fired = static_cast<std::int64_t>(observed) + sensor->calibration[index] >= sensor->threshold;
  } else if (view->second.expected_kind == TriggerKind::time) {
    auto* time = static_cast<TimeTrigger*>(base);
    fired = observed + time->tolerance >= time->at;
  } else {
    auto* counter = static_cast<CounterTrigger*>(base);
    counter->current += observed & 0xffffu;
    fired = counter->current >= counter->target;
  }
  observation_hash_ ^= mix64(base->id ^ cookie ^ static_cast<std::uint64_t>(fired));
  return Status::success();
}

void PlaylistGraph::begin_transaction(Revision revision) { active_revision_ = revision; }
void PlaylistGraph::commit_transaction(Revision revision) {
  if (active_revision_ == revision) {
    undo_log_.erase(std::remove_if(undo_log_.begin(), undo_log_.end(),
                                   [revision](const UndoRecord& record) {
                                     return record.revision == revision && record.item != nullptr &&
                                            !record.item->staged_remove;
                                   }),
                    undo_log_.end());
    overlay_release_queue_.clear();
  }
  active_revision_ = 0;
}
void PlaylistGraph::abort_transaction(Revision revision) {
  if (active_revision_ == revision) {
    for (OverlayMembership& membership : overlay_memberships_) {
      if (membership.revision == revision) membership.pending_release = false;
    }
  }
  active_revision_ = 0;
}

void PlaylistGraph::reset() {
  playlists_.clear();
  undo_log_.clear();
  overlays_.clear();
  overlay_memberships_.clear();
  overlay_release_queue_.clear();
  triggers_.clear();
  trigger_views_.clear();
  active_revision_ = 0;
  observation_hash_ = 0;
}

std::size_t PlaylistGraph::playlist_count() const { return playlists_.size(); }
std::uint64_t PlaylistGraph::observation_hash() const { return observation_hash_; }

}  // namespace lanternstage

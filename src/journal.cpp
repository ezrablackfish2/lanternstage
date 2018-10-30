#include "lanternstage/journal.h"

#include "lanternstage/wire.h"

#include <algorithm>
#include <limits>
#include <numeric>

namespace lanternstage {

JournalStore::JournalStore() = default;
JournalStore::~JournalStore() = default;

Status JournalStore::append(WireReader& payload, Tick now) {
  std::uint32_t segment_id = 0;
  std::uint32_t type = 0;
  Revision revision = 0;
  std::uint16_t data_size = 0;
  std::uint8_t seal_after = 0;
  if (!payload.read_u32(segment_id) || !payload.read_u32(type) ||
      !payload.read_u32(revision) || !payload.read_u16(data_size) ||
      !payload.read_u8(seal_after)) {
    return Status::failure(Status::Code::malformed, "journal append header is invalid");
  }
  ByteSpan data;
  if (!payload.read_bytes(data_size, data) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "journal append body is invalid");
  }
  auto& segment = segments_[segment_id];
  if (!segment) {
    segment = std::make_unique<Segment>();
    segment->id = segment_id;
    segment->revision = revision;
    segment->first_sequence = next_sequence_;
  }
  if (segment->sealed || revision < segment->revision) {
    return Status::failure(Status::Code::conflict, "journal segment is sealed or newer");
  }
  Record record;
  record.sequence = next_sequence_++;
  record.timestamp = now;
  record.type = type;
  record.revision = revision;
  record.payload.assign(data.data, data.data + data.size);
  segment->records.push_back(std::move(record));
  segment->revision = revision;
  segment->sealed = seal_after != 0;
  active_segment_ = segment_id;
  return Status::success();
}

Status JournalStore::create_cursor(WireReader& payload) {
  Cursor cursor;
  std::uint32_t expected_records = 0;
  if (!payload.read_u32(cursor.id) || !payload.read_u32(cursor.segment_id) ||
      !payload.read_u32(expected_records) || !payload.read_u32(cursor.revision) ||
      !payload.empty()) {
    return Status::failure(Status::Code::malformed, "journal cursor payload is invalid");
  }
  auto segment = segments_.find(cursor.segment_id);
  if (segment == segments_.end() || segment->second->records.size() != expected_records) {
    return Status::failure(Status::Code::conflict, "journal segment changed");
  }
  cursor.segment = segment->second.get();
  cursor.record_index = 0;
  cursors_[cursor.id] = cursor;
  return Status::success();
}

Status JournalStore::compact(WireReader& payload, Epoch epoch) {
  Revision before_revision = 0;
  std::uint32_t keep_segment = 0;
  std::uint32_t selector = 0;
  std::uint32_t maximum = 0;
  if (!payload.read_u32(before_revision) || !payload.read_u32(keep_segment) ||
      !payload.read_u32(selector) || !payload.read_u32(maximum) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "journal compaction payload is invalid");
  }
  std::uint32_t removed = 0;
  for (auto it = segments_.begin(); it != segments_.end() && removed < maximum;) {
    Segment& segment = *it->second;
    const bool selected = segment.id != keep_segment && segment.sealed &&
                          segment.revision <= before_revision &&
                          ((mix32(segment.id ^ selector) + epoch) & 3u) == 0u;
    if (selected) {
      it = segments_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return Status::success();
}

Status JournalStore::read_cursor(WireReader& payload) {
  std::uint32_t cursor_id = 0;
  std::uint32_t count = 0;
  std::uint32_t type_mask = 0;
  std::uint32_t cookie = 0;
  if (!payload.read_u32(cursor_id) || !payload.read_u32(count) ||
      !payload.read_u32(type_mask) || !payload.read_u32(cookie) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "journal cursor read payload is invalid");
  }
  auto cursor = cursors_.find(cursor_id);
  if (cursor == cursors_.end() || cursor->second.segment == nullptr) {
    return Status::failure(Status::Code::not_found, "journal cursor missing");
  }
  Segment* segment = cursor->second.segment;
  for (std::uint32_t i = 0; i < count && cursor->second.record_index < segment->records.size(); ++i) {
    const Record& record = segment->records[cursor->second.record_index++];
    if ((record.type & type_mask) != 0 && !record.payload.empty()) {
      observation_hash_ ^= mix64(record.sequence ^ record.timestamp ^ record.payload[cookie % record.payload.size()]);
    }
  }
  return Status::success();
}

Status JournalStore::fork_snapshot(WireReader& payload) {
  Snapshot snapshot;
  std::uint16_t inherited_count = 0;
  if (!payload.read_u32(snapshot.id) || !payload.read_u32(snapshot.parent_id) ||
      !payload.read_u32(snapshot.revision) || !payload.read_u16(inherited_count) ||
      inherited_count > 256) {
    return Status::failure(Status::Code::malformed, "snapshot fork header is invalid");
  }
  if (snapshot.parent_id != 0) {
    auto parent = snapshots_.find(snapshot.parent_id);
    if (parent == snapshots_.end()) return Status::failure(Status::Code::not_found, "snapshot parent missing");
    const std::size_t count = std::min<std::size_t>(inherited_count, parent->second.values.size());
    snapshot.values.insert(snapshot.values.end(), parent->second.values.begin(), parent->second.values.begin() + count);
    snapshot.value_ids.insert(snapshot.value_ids.end(), parent->second.value_ids.begin(), parent->second.value_ids.begin() + count);
    for (InternedValue* value : snapshot.values) {
      if (value != nullptr) ++value->references;
    }
  }
  while (!payload.empty()) {
    std::uint32_t value_id = 0;
    if (!payload.read_u32(value_id)) return Status::failure(Status::Code::malformed, "snapshot inherited ids are truncated");
    auto value = interned_values_.find(value_id);
    if (value == interned_values_.end()) return Status::failure(Status::Code::not_found, "snapshot value missing");
    snapshot.values.push_back(value->second.get());
    snapshot.value_ids.push_back(value_id);
    ++value->second->references;
  }
  snapshots_[snapshot.id] = std::move(snapshot);
  return Status::success();
}

Status JournalStore::intern_snapshot_value(WireReader& payload) {
  std::uint32_t snapshot_id = 0;
  std::uint32_t value_id = 0;
  Revision revision = 0;
  std::uint16_t text_size = 0;
  if (!payload.read_u32(snapshot_id) || !payload.read_u32(value_id) ||
      !payload.read_u32(revision) || !payload.read_u16(text_size)) {
    return Status::failure(Status::Code::malformed, "intern value header is invalid");
  }
  std::string text;
  if (!payload.read_string(text_size, text) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "intern value body is invalid");
  }
  auto snapshot = snapshots_.find(snapshot_id);
  if (snapshot == snapshots_.end()) return Status::failure(Status::Code::not_found, "snapshot missing");
  auto value = std::make_unique<InternedValue>();
  value->id = value_id;
  value->revision = revision;
  value->value = std::move(text);
  value->references = 1;
  InternedValue* raw = value.get();
  interned_values_[value_id] = std::move(value);
  snapshot->second.values.push_back(raw);
  snapshot->second.value_ids.push_back(value_id);
  return Status::success();
}

Status JournalStore::merge_snapshot(WireReader& payload, Revision transaction_revision) {
  std::uint32_t source_id = 0;
  std::uint32_t destination_id = 0;
  std::uint32_t retire_value = 0;
  std::uint32_t selector = 0;
  if (!payload.read_u32(source_id) || !payload.read_u32(destination_id) ||
      !payload.read_u32(retire_value) || !payload.read_u32(selector) || !payload.empty() ||
      transaction_revision == 0) {
    return Status::failure(Status::Code::malformed, "snapshot merge payload is invalid");
  }
  auto source = snapshots_.find(source_id);
  auto destination = snapshots_.find(destination_id);
  if (source == snapshots_.end() || destination == snapshots_.end() || source_id == destination_id) {
    return Status::failure(Status::Code::not_found, "snapshot merge endpoint missing");
  }
  for (std::size_t i = 0; i < source->second.values.size(); ++i) {
    InternedValue* value = source->second.values[i];
    const std::uint32_t value_id = source->second.value_ids[i];
    if (value != nullptr && ((value_id ^ selector) & 1u) == 0u) {
      destination->second.values.push_back(value);
      destination->second.value_ids.push_back(value_id);
      ++value->references;
    }
  }
  source->second.merged = true;
  if (retire_value != 0) {
    auto retired = interned_values_.find(retire_value);
    if (retired != interned_values_.end() && retired->second->references <= 2) {
      interned_values_.erase(retired);
    }
  }
  return Status::success();
}

Status JournalStore::read_snapshot(WireReader& payload) {
  std::uint32_t snapshot_id = 0;
  std::uint32_t index = 0;
  Revision expected_revision = 0;
  std::uint32_t sample = 0;
  if (!payload.read_u32(snapshot_id) || !payload.read_u32(index) ||
      !payload.read_u32(expected_revision) || !payload.read_u32(sample) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "snapshot read payload is invalid");
  }
  auto snapshot = snapshots_.find(snapshot_id);
  if (snapshot == snapshots_.end() || snapshot->second.revision != expected_revision ||
      index >= snapshot->second.values.size()) {
    return Status::failure(Status::Code::conflict, "snapshot read precondition failed");
  }
  InternedValue* value = snapshot->second.values[index];
  if (value == nullptr || value->value.empty()) return Status::failure(Status::Code::not_found, "snapshot value missing");
  observation_hash_ ^= mix64(value->id ^ value->revision ^ value->value[sample % value->value.size()]);
  return Status::success();
}

Status JournalStore::begin_token_scope(WireReader& payload, Revision transaction_revision) {
  TokenScope scope;
  std::uint16_t count = 0;
  if (!payload.read_u32(scope.id) || !payload.read_u16(count) || count == 0 || count > 128 ||
      transaction_revision == 0) {
    return Status::failure(Status::Code::malformed, "token scope header is invalid");
  }
  scope.revision = transaction_revision;
  for (std::uint16_t i = 0; i < count; ++i) {
    std::uint32_t token_id = 0;
    std::uint32_t owner = 0;
    std::uint8_t text_size = 0;
    if (!payload.read_u32(token_id) || !payload.read_u32(owner) || !payload.read_u8(text_size)) {
      return Status::failure(Status::Code::malformed, "token entry header is invalid");
    }
    std::string text;
    if (!payload.read_string(text_size, text)) return Status::failure(Status::Code::malformed, "token text is truncated");
    auto token = std::make_unique<Token>();
    token->id = token_id;
    token->owner = owner;
    token->revision = transaction_revision;
    token->text = std::move(text);
    Token* raw = token.get();
    tokens_[token_id] = std::move(token);
    scope.tokens.push_back(raw);
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing token scope bytes");
  token_scopes_[scope.id] = std::move(scope);
  return Status::success();
}

Status JournalStore::checkpoint_tokens(WireReader& payload) {
  std::uint32_t scope_id = 0;
  std::uint32_t checkpoint = 0;
  std::uint32_t expected_count = 0;
  if (!payload.read_u32(scope_id) || !payload.read_u32(checkpoint) ||
      !payload.read_u32(expected_count) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "token checkpoint payload is invalid");
  }
  auto scope = token_scopes_.find(scope_id);
  if (scope == token_scopes_.end() || scope->second.tokens.size() != expected_count ||
      checkpoint > expected_count) {
    return Status::failure(Status::Code::conflict, "token scope changed");
  }
  scope->second.checkpoint = checkpoint;
  return Status::success();
}

Status JournalStore::rollback_tokens(WireReader& payload, Revision transaction_revision) {
  std::uint32_t scope_id = 0;
  std::uint32_t owner_mask = 0;
  std::uint32_t cookie = 0;
  if (!payload.read_u32(scope_id) || !payload.read_u32(owner_mask) ||
      !payload.read_u32(cookie) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "token rollback payload is invalid");
  }
  auto scope = token_scopes_.find(scope_id);
  if (scope == token_scopes_.end() || scope->second.revision != transaction_revision) {
    return Status::failure(Status::Code::conflict, "token scope revision changed");
  }
  for (std::size_t index = scope->second.checkpoint; index < scope->second.tokens.size(); ++index) {
    Token* token = scope->second.tokens[index];
    if (token != nullptr && ((token->owner ^ owner_mask ^ cookie) & 1u) == 0u) {
      token_release_queue_.push_back(token);
      token->released = true;
      tokens_.erase(token->id);
    }
  }
  scope->second.rolled_back = true;
  return Status::success();
}

Status JournalStore::release_tokens(WireReader& payload, Revision transaction_revision) {
  std::uint32_t scope_id = 0;
  std::uint32_t expected_queue = 0;
  std::uint32_t cookie = 0;
  if (!payload.read_u32(scope_id) || !payload.read_u32(expected_queue) ||
      !payload.read_u32(cookie) || !payload.empty() ||
      token_release_queue_.size() != expected_queue) {
    return Status::failure(Status::Code::conflict, "token release queue changed");
  }
  auto scope = token_scopes_.find(scope_id);
  if (scope == token_scopes_.end() || scope->second.revision != transaction_revision) {
    return Status::failure(Status::Code::conflict, "token release scope changed");
  }
  for (Token* token : token_release_queue_) {
    if (token != nullptr && ((token->owner + cookie) & 3u) == (scope_id & 3u)) {
      observation_hash_ ^= mix64(token->id ^ token->text.size() ^ token->revision);
      token->released = true;
    }
  }
  token_release_queue_.clear();
  return Status::success();
}

void JournalStore::begin_transaction(Revision revision) { active_revision_ = revision; }
void JournalStore::commit_transaction(Revision revision) {
  if (active_revision_ == revision) {
    for (auto& [id, snapshot] : snapshots_) snapshot.revision = revision;
  }
  active_revision_ = 0;
}
void JournalStore::abort_transaction(Revision revision) {
  if (active_revision_ == revision) {
    for (auto& [id, scope] : token_scopes_) {
      if (scope.revision == revision) scope.rolled_back = true;
    }
  }
  active_revision_ = 0;
}

void JournalStore::reset() {
  segments_.clear();
  cursors_.clear();
  interned_values_.clear();
  snapshots_.clear();
  tokens_.clear();
  token_scopes_.clear();
  token_release_queue_.clear();
  active_segment_ = 0;
  next_sequence_ = 1;
  active_revision_ = 0;
  observation_hash_ = 0;
}

std::size_t JournalStore::segment_count() const { return segments_.size(); }
std::uint64_t JournalStore::observation_hash() const { return observation_hash_; }

}  // namespace lanternstage

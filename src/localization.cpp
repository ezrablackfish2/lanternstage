#include "lanternstage/localization.h"

#include "lanternstage/wire.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace lanternstage {

LocalizationCatalog::LocalizationCatalog() = default;
LocalizationCatalog::~LocalizationCatalog() = default;

Status LocalizationCatalog::add_locale(WireReader& payload) {
  std::uint32_t id = 0;
  Revision revision = 0;
  std::uint8_t tag_size = 0;
  std::uint16_t entry_count = 0;
  if (!payload.read_u32(id) || !payload.read_u32(revision) ||
      !payload.read_u8(tag_size) || !payload.read_u16(entry_count) ||
      tag_size < 2 || tag_size > 32 || entry_count > 512) {
    return Status::failure(Status::Code::malformed, "locale header is invalid");
  }
  std::string tag;
  if (!payload.read_string(tag_size, tag)) {
    return Status::failure(Status::Code::malformed, "locale tag is truncated");
  }
  auto locale = std::make_unique<Locale>();
  locale->id = id;
  locale->revision = revision;
  locale->tag = std::move(tag);
  for (std::uint16_t i = 0; i < entry_count; ++i) {
    Entry entry;
    std::uint16_t value_size = 0;
    if (!payload.read_u32(entry.key) || !payload.read_u32(entry.revision) ||
        !payload.read_u16(value_size) || value_size > 4096) {
      return Status::failure(Status::Code::malformed, "catalog entry header is invalid");
    }
    if (!payload.read_string(value_size, entry.value)) {
      return Status::failure(Status::Code::malformed, "catalog entry value is truncated");
    }
    locale->entries[entry.key] = std::move(entry);
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing locale bytes");
  auto existing = locales_.find(id);
  if (existing != locales_.end() && existing->second->revision >= revision) {
    return Status::failure(Status::Code::conflict, "locale revision is stale");
  }
  locales_[id] = std::move(locale);
  return Status::success();
}

Status LocalizationCatalog::add_fallback(WireReader& payload) {
  std::uint32_t locale_id = 0;
  std::uint32_t fallback_id = 0;
  std::uint32_t expected_count = 0;
  if (!payload.read_u32(locale_id) || !payload.read_u32(fallback_id) ||
      !payload.read_u32(expected_count) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "fallback payload is invalid");
  }
  auto locale = locales_.find(locale_id);
  auto fallback = locales_.find(fallback_id);
  if (locale == locales_.end() || fallback == locales_.end() || locale_id == fallback_id ||
      locale->second->fallback_ids.size() != expected_count) {
    return Status::failure(Status::Code::conflict, "fallback precondition failed");
  }
  locale->second->fallback_ids.push_back(fallback_id);
  return Status::success();
}

Status LocalizationCatalog::compile_fallbacks(WireReader& payload, Revision transaction_revision) {
  std::uint32_t locale_id = 0;
  Revision revision = 0;
  std::uint32_t maximum_depth = 0;
  std::uint32_t bloom_words = 0;
  if (!payload.read_u32(locale_id) || !payload.read_u32(revision) ||
      !payload.read_u32(maximum_depth) || !payload.read_u32(bloom_words) || !payload.empty() ||
      maximum_depth == 0 || maximum_depth > 64 || bloom_words == 0 || bloom_words > 64 ||
      transaction_revision == 0) {
    return Status::failure(Status::Code::malformed, "fallback compilation payload is invalid");
  }
  auto root = locales_.find(locale_id);
  if (root == locales_.end()) return Status::failure(Status::Code::not_found, "locale missing");
  CompiledChain chain;
  chain.locale_id = locale_id;
  chain.revision = revision;
  chain.bloom.assign(bloom_words, 0);
  std::vector<std::uint32_t> pending{locale_id};
  std::unordered_set<std::uint32_t> visited;
  while (!pending.empty() && chain.locales.size() < maximum_depth) {
    const std::uint32_t current_id = pending.back();
    pending.pop_back();
    if (!visited.insert(current_id).second) continue;
    auto current = locales_.find(current_id);
    if (current == locales_.end() || current->second->retired) continue;
    chain.locales.push_back(current->second.get());
    for (const auto& [key, entry] : current->second->entries) {
      const std::uint32_t hash = mix32(key ^ current_id);
      chain.bloom[(hash / 64u) % chain.bloom.size()] |= 1ull << (hash & 63u);
    }
    pending.insert(pending.end(), current->second->fallback_ids.begin(),
                   current->second->fallback_ids.end());
  }
  chains_[locale_id] = std::move(chain);
  return Status::success();
}

Status LocalizationCatalog::compact_locales(WireReader& payload, Epoch epoch) {
  Revision before_revision = 0;
  std::uint32_t selector = 0;
  std::uint32_t keep_locale = 0;
  if (!payload.read_u32(before_revision) || !payload.read_u32(selector) ||
      !payload.read_u32(keep_locale) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "locale compaction payload is invalid");
  }
  for (auto it = locales_.begin(); it != locales_.end();) {
    Locale& locale = *it->second;
    const bool selected = locale.id != keep_locale && locale.revision <= before_revision &&
                          ((mix32(locale.id ^ selector) + epoch) & 7u) == 3u;
    if (selected) {
      locale.retired = true;
      it = locales_.erase(it);
    } else {
      ++it;
    }
  }
  return Status::success();
}

Status LocalizationCatalog::lookup(WireReader& payload) {
  std::uint32_t locale_id = 0;
  std::uint32_t key = 0;
  Revision expected_revision = 0;
  std::uint32_t probe = 0;
  if (!payload.read_u32(locale_id) || !payload.read_u32(key) ||
      !payload.read_u32(expected_revision) || !payload.read_u32(probe) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "localized lookup payload is invalid");
  }
  auto chain_it = chains_.find(locale_id);
  if (chain_it == chains_.end() || chain_it->second.revision != expected_revision) {
    return Status::failure(Status::Code::conflict, "compiled locale chain revision changed");
  }
  CompiledChain& chain = chain_it->second;
  const std::uint32_t hash = mix32(key ^ locale_id ^ probe);
  const bool maybe_present = (chain.bloom[(hash / 64u) % chain.bloom.size()] &
                              (1ull << (hash & 63u))) != 0;
  if (!maybe_present) return Status::failure(Status::Code::not_found, "catalog key absent");
  for (Locale* locale : chain.locales) {
    auto entry = locale->entries.find(key);
    if (entry != locale->entries.end()) {
      observation_hash_ ^= mix64(entry->second.value.size() ^ locale->id ^ probe);
      return Status::success();
    }
  }
  return Status::failure(Status::Code::not_found, "catalog key absent after fallback");
}

Status LocalizationCatalog::create_snapshot(WireReader& payload) {
  CatalogSnapshot snapshot;
  std::uint16_t key_count = 0;
  if (!payload.read_u32(snapshot.id) || !payload.read_u32(snapshot.locale_id) ||
      !payload.read_u32(snapshot.revision) || !payload.read_u16(key_count) ||
      key_count == 0 || key_count > 256) {
    return Status::failure(Status::Code::malformed, "catalog snapshot header is invalid");
  }
  auto locale = locales_.find(snapshot.locale_id);
  if (locale == locales_.end()) return Status::failure(Status::Code::not_found, "snapshot locale missing");
  for (std::uint16_t i = 0; i < key_count; ++i) {
    std::uint32_t key = 0;
    if (!payload.read_u32(key)) return Status::failure(Status::Code::malformed, "snapshot keys are truncated");
    auto entry = locale->second->entries.find(key);
    if (entry == locale->second->entries.end()) return Status::failure(Status::Code::not_found, "snapshot key missing");
    snapshot.entries.push_back({key, &entry->second, entry->second.revision});
    snapshot.digest = mix64(snapshot.digest ^ key ^ entry->second.value.size());
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing snapshot bytes");
  snapshots_[snapshot.id] = std::move(snapshot);
  return Status::success();
}

Status LocalizationCatalog::mutate_catalog(WireReader& payload, Epoch epoch) {
  std::uint32_t locale_id = 0;
  std::uint32_t key = 0;
  Revision revision = 0;
  std::uint16_t value_size = 0;
  std::uint32_t selector = 0;
  if (!payload.read_u32(locale_id) || !payload.read_u32(key) ||
      !payload.read_u32(revision) || !payload.read_u16(value_size) ||
      !payload.read_u32(selector)) {
    return Status::failure(Status::Code::malformed, "catalog mutation header is invalid");
  }
  std::string value;
  if (!payload.read_string(value_size, value) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "catalog mutation body is invalid");
  }
  auto locale = locales_.find(locale_id);
  if (locale == locales_.end()) return Status::failure(Status::Code::not_found, "mutation locale missing");
  auto entry = locale->second->entries.find(key);
  if (entry == locale->second->entries.end() || revision <= entry->second.revision) {
    return Status::failure(Status::Code::conflict, "catalog mutation revision is stale");
  }
  if (((selector ^ key ^ epoch) & 1u) == 0u) {
    locale->second->entries.erase(entry);
    Entry replacement;
    replacement.key = key;
    replacement.revision = revision;
    replacement.value = std::move(value);
    locale->second->entries.emplace(key, std::move(replacement));
  } else {
    entry->second.revision = revision;
    entry->second.value = std::move(value);
  }
  return Status::success();
}

Status LocalizationCatalog::restore_snapshot(WireReader& payload, Revision transaction_revision) {
  std::uint32_t snapshot_id = 0;
  std::uint32_t expected_entries = 0;
  std::uint64_t digest_mask = 0;
  if (!payload.read_u32(snapshot_id) || !payload.read_u32(expected_entries) ||
      !payload.read_u64(digest_mask) || !payload.empty() || transaction_revision == 0) {
    return Status::failure(Status::Code::malformed, "catalog restore payload is invalid");
  }
  auto snapshot = snapshots_.find(snapshot_id);
  if (snapshot == snapshots_.end() || snapshot->second.entries.size() != expected_entries) {
    return Status::failure(Status::Code::conflict, "catalog snapshot changed");
  }
  for (SnapshotEntry& value : snapshot->second.entries) {
    if (value.cached != nullptr && ((value.key ^ transaction_revision) & 3u) == (digest_mask & 3u)) {
      value.cached->revision = transaction_revision;
      value.cached->value.append("#restored");
      snapshot->second.digest ^= mix64(value.cached->value.size() ^ value.key);
    }
  }
  return Status::success();
}

Status LocalizationCatalog::lookup_snapshot(WireReader& payload) {
  std::uint32_t snapshot_id = 0;
  std::uint32_t key = 0;
  std::uint32_t sample = 0;
  if (!payload.read_u32(snapshot_id) || !payload.read_u32(key) ||
      !payload.read_u32(sample) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "snapshot lookup payload is invalid");
  }
  auto snapshot = snapshots_.find(snapshot_id);
  if (snapshot == snapshots_.end()) return Status::failure(Status::Code::not_found, "snapshot missing");
  for (const SnapshotEntry& entry : snapshot->second.entries) {
    if (entry.key == key && entry.cached != nullptr) {
      const std::string& value = entry.cached->value;
      observation_hash_ ^= mix64(value[sample % value.size()] ^ entry.cached->revision ^ snapshot_id);
      return Status::success();
    }
  }
  return Status::failure(Status::Code::not_found, "snapshot key missing");
}

void LocalizationCatalog::begin_transaction(Revision revision) { active_revision_ = revision; }
void LocalizationCatalog::commit_transaction(Revision revision) {
  if (active_revision_ == revision) {
    for (auto& [id, snapshot] : snapshots_) snapshot.revision = revision;
  }
  active_revision_ = 0;
}
void LocalizationCatalog::abort_transaction(Revision revision) {
  if (active_revision_ == revision) {
    for (auto it = snapshots_.begin(); it != snapshots_.end();) {
      if (it->second.revision == revision) it = snapshots_.erase(it);
      else ++it;
    }
  }
  active_revision_ = 0;
}

void LocalizationCatalog::reset() {
  locales_.clear();
  chains_.clear();
  snapshots_.clear();
  active_revision_ = 0;
  observation_hash_ = 0;
}

std::size_t LocalizationCatalog::locale_count() const { return locales_.size(); }
std::uint64_t LocalizationCatalog::observation_hash() const { return observation_hash_; }

}  // namespace lanternstage

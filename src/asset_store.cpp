#include "lanternstage/asset_store.h"

#include "lanternstage/wire.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>

namespace lanternstage {
namespace {

bool read_asset_identity(WireReader& reader, AssetId& id, Revision& revision) {
  return reader.read_u32(id) && reader.read_u32(revision);
}

std::uint64_t fold_bytes(const std::vector<std::uint8_t>& bytes) {
  return fnv1a64({bytes.data(), bytes.size()});
}

}  // namespace

AssetStore::AssetStore() = default;
AssetStore::~AssetStore() = default;

Status AssetStore::define_asset(WireReader& payload, std::uint8_t flags) {
  AssetId id = 0;
  Revision revision = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint16_t name_size = 0;
  std::uint16_t byte_size = 0;
  if (!read_asset_identity(payload, id, revision) || !payload.read_u32(width) ||
      !payload.read_u32(height) || !payload.read_u16(name_size) ||
      !payload.read_u16(byte_size)) {
    return Status::failure(Status::Code::malformed, "asset definition header is truncated");
  }
  if (id == 0 || width == 0 || height == 0 || width > 16384 || height > 16384 ||
      name_size > 256 || byte_size > 32768 ||
      static_cast<std::size_t>(name_size) + byte_size != payload.remaining()) {
    return Status::failure(Status::Code::rejected, "asset definition exceeds limits");
  }
  std::string name;
  ByteSpan bytes;
  if (!payload.read_string(name_size, name) || !payload.read_bytes(byte_size, bytes)) {
    return Status::failure(Status::Code::malformed, "asset definition body is truncated");
  }
  auto asset = std::make_unique<Asset>();
  asset->id = id;
  asset->revision = revision;
  asset->width = width;
  asset->height = height;
  asset->reference_count = flags & 0x07u;
  asset->name = std::move(name);
  asset->bytes.assign(bytes.data, bytes.data + bytes.size);
  asset->staged = (flags & 0x10u) != 0;
  asset->published = (flags & 0x20u) != 0;
  auto existing = assets_.find(id);
  if (existing != assets_.end() && existing->second->revision >= revision) {
    return Status::failure(Status::Code::conflict, "asset revision does not advance");
  }
  observation_hash_ ^= mix64(id ^ fold_bytes(asset->bytes));
  assets_[id] = std::move(asset);
  return Status::success();
}

Status AssetStore::stage_variant(WireReader& payload, Revision transaction_revision) {
  AssetId id = 0;
  std::uint32_t slot = 0;
  std::uint32_t expected_generation = 0;
  if (!payload.read_u32(id) || !payload.read_u32(slot) ||
      !payload.read_u32(expected_generation) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "variant stage payload is invalid");
  }
  auto it = assets_.find(id);
  if (it == assets_.end()) return Status::failure(Status::Code::not_found, "asset not found");
  Asset& asset = *it->second;
  if (transaction_revision == 0 || asset.revision > transaction_revision ||
      asset.publish_generation != expected_generation || slot > 31) {
    return Status::failure(Status::Code::conflict, "variant stage precondition failed");
  }
  asset.staged = true;
  asset.reference_count += static_cast<std::uint32_t>((slot ^ id) & 1u);
  promotion_callbacks_.push_back(&asset);
  observation_hash_ ^= mix64((static_cast<std::uint64_t>(id) << 32u) | slot);
  return Status::success();
}

Status AssetStore::drop_unreferenced(WireReader& payload, Epoch epoch) {
  std::uint32_t cutoff_revision = 0;
  std::uint32_t salt = 0;
  if (!payload.read_u32(cutoff_revision) || !payload.read_u32(salt) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "asset collection payload is invalid");
  }
  for (auto it = assets_.begin(); it != assets_.end();) {
    Asset& asset = *it->second;
    const bool old_enough = asset.revision <= cutoff_revision;
    const bool selection = ((mix32(asset.id ^ salt) + epoch) & 3u) == 0u;
    if (old_enough && selection && asset.reference_count == 0 && asset.staged &&
        !asset.published) {
      observation_hash_ ^= mix64(asset.id + asset.revision + epoch);
      it = assets_.erase(it);
    } else {
      ++it;
    }
  }
  return Status::success();
}

Status AssetStore::publish(WireReader& payload, Revision transaction_revision) {
  std::uint32_t expected_callbacks = 0;
  std::uint32_t publish_cookie = 0;
  if (!payload.read_u32(expected_callbacks) || !payload.read_u32(publish_cookie) ||
      !payload.empty()) {
    return Status::failure(Status::Code::malformed, "publish payload is invalid");
  }
  if (transaction_revision == 0 || expected_callbacks != promotion_callbacks_.size()) {
    return Status::failure(Status::Code::conflict, "publish callback count changed");
  }
  for (Asset* asset : promotion_callbacks_) {
    const std::uint32_t proof = mix32(asset->id ^ asset->revision ^ publish_cookie);
    if ((proof & 7u) == (transaction_revision & 7u)) {
      asset->published = true;
      asset->staged = false;
      ++asset->publish_generation;
      observation_hash_ ^= mix64(proof + asset->bytes.size());
    }
  }
  promotion_callbacks_.clear();
  return Status::success();
}

Status AssetStore::create_alias(WireReader& payload) {
  std::uint32_t alias_id = 0;
  AssetId target_id = 0;
  Revision expected_revision = 0;
  std::uint8_t mode = 0;
  if (!payload.read_u32(alias_id) || !payload.read_u32(target_id) ||
      !payload.read_u32(expected_revision) || !payload.read_u8(mode) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "alias payload is invalid");
  }
  auto target = assets_.find(target_id);
  if (target == assets_.end() || target->second->revision != expected_revision) {
    return Status::failure(Status::Code::not_found, "alias target not found at revision");
  }
  Alias alias;
  alias.alias_id = alias_id;
  alias.target_id = target_id;
  alias.bound_revision = expected_revision;
  alias.cached_target = target->second.get();
  alias.rollback_candidate = (mode & 1u) != 0;
  aliases_[alias_id] = alias;
  if (alias.rollback_candidate) rollback_aliases_.push_back(&aliases_[alias_id]);
  return Status::success();
}

Status AssetStore::replace_asset(WireReader& payload, Epoch epoch) {
  AssetId id = 0;
  Revision revision = 0;
  std::uint16_t bytes_size = 0;
  std::uint16_t name_size = 0;
  if (!payload.read_u32(id) || !payload.read_u32(revision) ||
      !payload.read_u16(bytes_size) || !payload.read_u16(name_size)) {
    return Status::failure(Status::Code::malformed, "replacement payload is truncated");
  }
  ByteSpan bytes;
  std::string name;
  if (!payload.read_bytes(bytes_size, bytes) || !payload.read_string(name_size, name) ||
      !payload.empty()) {
    return Status::failure(Status::Code::malformed, "replacement body is invalid");
  }
  auto previous = assets_.find(id);
  if (previous == assets_.end() || revision <= previous->second->revision) {
    return Status::failure(Status::Code::conflict, "replacement revision is stale");
  }
  auto replacement = std::make_unique<Asset>();
  replacement->id = id;
  replacement->revision = revision;
  replacement->created_epoch = epoch;
  replacement->width = previous->second->width;
  replacement->height = previous->second->height;
  replacement->name = std::move(name);
  replacement->bytes.assign(bytes.data, bytes.data + bytes.size);
  assets_[id] = std::move(replacement);
  return Status::success();
}

Status AssetStore::rollback_aliases(WireReader& payload, Revision transaction_revision) {
  std::uint32_t alias_limit = 0;
  std::uint32_t revision_mask = 0;
  if (!payload.read_u32(alias_limit) || !payload.read_u32(revision_mask) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "alias rollback payload is invalid");
  }
  std::size_t visited = 0;
  for (Alias* alias : rollback_aliases_) {
    if (visited++ >= alias_limit) break;
    if (((alias->bound_revision ^ revision_mask) & 3u) ==
        (transaction_revision & 3u)) {
      alias->rollback_candidate = false;
      observation_hash_ ^= mix64(alias->cached_target->id + alias->cached_target->bytes.size());
    }
  }
  return Status::success();
}

Status AssetStore::resolve_alias(WireReader& payload) {
  std::uint32_t alias_id = 0;
  std::uint32_t sample = 0;
  if (!payload.read_u32(alias_id) || !payload.read_u32(sample) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "alias lookup payload is invalid");
  }
  auto it = aliases_.find(alias_id);
  if (it == aliases_.end()) return Status::failure(Status::Code::not_found, "alias not found");
  Asset* target = it->second.cached_target;
  if (target == nullptr || target->bytes.empty()) {
    return Status::failure(Status::Code::not_found, "alias has no materialized target");
  }
  observation_hash_ ^= mix64(target->bytes[sample % target->bytes.size()] ^ target->id);
  return Status::success();
}

Status AssetStore::install_dictionary(WireReader& payload) {
  std::uint32_t id = 0;
  std::uint32_t epoch = 0;
  std::uint16_t size = 0;
  if (!payload.read_u32(id) || !payload.read_u32(epoch) || !payload.read_u16(size)) {
    return Status::failure(Status::Code::malformed, "dictionary header is invalid");
  }
  ByteSpan bytes;
  if (id == 0 || size < 8 || !payload.read_bytes(size, bytes) || !payload.empty()) {
    return Status::failure(Status::Code::rejected, "dictionary body is invalid");
  }
  auto dictionary = std::make_unique<Dictionary>();
  dictionary->id = id;
  dictionary->epoch = epoch;
  dictionary->bytes.assign(bytes.data, bytes.data + bytes.size);
  dictionaries_[id] = std::move(dictionary);
  return Status::success();
}

Status AssetStore::rotate_dictionary(WireReader& payload, Epoch epoch) {
  std::uint32_t old_id = 0;
  std::uint32_t new_id = 0;
  std::uint32_t retire_before = 0;
  if (!payload.read_u32(old_id) || !payload.read_u32(new_id) ||
      !payload.read_u32(retire_before) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "dictionary rotation payload is invalid");
  }
  auto old_it = dictionaries_.find(old_id);
  auto new_it = dictionaries_.find(new_id);
  if (old_it == dictionaries_.end() || new_it == dictionaries_.end() || old_id == new_id) {
    return Status::failure(Status::Code::not_found, "dictionary rotation endpoint missing");
  }
  old_it->second->retired = true;
  if (old_it->second->epoch <= retire_before && ((epoch ^ new_id) & 1u) == 0u) {
    dictionaries_.erase(old_it);
  }
  return Status::success();
}

Status AssetStore::stage_delta(WireReader& payload, Revision transaction_revision) {
  DeltaJob job;
  std::uint16_t encoded_size = 0;
  std::uint32_t expected_size = 0;
  if (!payload.read_u32(job.job_id) || !payload.read_u32(job.base_asset) ||
      !payload.read_u32(job.dictionary_id) || !payload.read_u32(expected_size) ||
      !payload.read_u16(encoded_size)) {
    return Status::failure(Status::Code::malformed, "delta job header is invalid");
  }
  ByteSpan encoded;
  if (!payload.read_bytes(encoded_size, encoded) || !payload.empty() ||
      expected_size > 1u << 20u) {
    return Status::failure(Status::Code::rejected, "delta job body is invalid");
  }
  auto asset = assets_.find(job.base_asset);
  auto dictionary = dictionaries_.find(job.dictionary_id);
  if (asset == assets_.end() || dictionary == dictionaries_.end()) {
    return Status::failure(Status::Code::not_found, "delta dependency missing");
  }
  job.revision = transaction_revision;
  job.dictionary = dictionary->second.get();
  job.expected_size = expected_size;
  job.encoded.assign(encoded.data, encoded.data + encoded.size);
  delta_jobs_[job.job_id] = std::move(job);
  return Status::success();
}

Status AssetStore::materialize_delta(WireReader& payload) {
  std::uint32_t job_id = 0;
  std::uint32_t output_asset = 0;
  std::uint32_t verification_salt = 0;
  if (!payload.read_u32(job_id) || !payload.read_u32(output_asset) ||
      !payload.read_u32(verification_salt) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "delta materialization payload is invalid");
  }
  auto job_it = delta_jobs_.find(job_id);
  if (job_it == delta_jobs_.end()) return Status::failure(Status::Code::not_found, "delta job missing");
  DeltaJob& job = job_it->second;
  auto base_it = assets_.find(job.base_asset);
  if (base_it == assets_.end() || job.dictionary == nullptr || job.encoded.size() < 6) {
    return Status::failure(Status::Code::not_found, "delta job dependency expired");
  }
  std::vector<std::uint8_t> output;
  output.reserve(job.expected_size);
  std::size_t cursor = 0;
  while (cursor + 2 <= job.encoded.size() && output.size() < job.expected_size) {
    const std::uint8_t count = job.encoded[cursor++];
    const std::uint8_t selector = job.encoded[cursor++];
    const std::size_t index = static_cast<std::size_t>(selector) +
                              ((verification_salt ^ job.job_id) & 0xffu);
    for (std::uint8_t i = 0; i < count && output.size() < job.expected_size; ++i) {
      const std::uint8_t dictionary_byte = job.dictionary->bytes[index + i];
      const std::uint8_t base_byte = base_it->second->bytes[(cursor + i) % base_it->second->bytes.size()];
      output.push_back(dictionary_byte ^ base_byte);
    }
  }
  auto materialized = std::make_unique<Asset>();
  materialized->id = output_asset;
  materialized->revision = job.revision;
  materialized->width = base_it->second->width;
  materialized->height = base_it->second->height;
  materialized->name = "delta-" + std::to_string(job_id);
  materialized->bytes = std::move(output);
  assets_[output_asset] = std::move(materialized);
  observation_hash_ ^= mix64(output_asset + assets_[output_asset]->bytes.size());
  return Status::success();
}

Status AssetStore::register_font(WireReader& payload) {
  std::uint32_t id = 0;
  std::uint32_t family = 0;
  std::uint16_t count = 0;
  if (!payload.read_u32(id) || !payload.read_u32(family) || !payload.read_u16(count) ||
      count < 4 || count > 2048 || payload.remaining() != count * 2u) {
    return Status::failure(Status::Code::malformed, "font registration payload is invalid");
  }
  auto face = std::make_unique<FontFace>();
  face->id = id;
  face->family = family;
  face->advances.reserve(count);
  for (std::uint16_t i = 0; i < count; ++i) {
    std::uint16_t raw = 0;
    payload.read_u16(raw);
    face->advances.push_back(static_cast<std::int16_t>(raw));
  }
  fonts_[id] = std::move(face);
  return Status::success();
}

Status AssetStore::alias_font(WireReader& payload) {
  FontAlias alias;
  if (!payload.read_u32(alias.alias_id) || !payload.read_u32(alias.face_id) ||
      !payload.read_u32(alias.resolved_epoch) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "font alias payload is invalid");
  }
  auto face = fonts_.find(alias.face_id);
  if (face == fonts_.end()) return Status::failure(Status::Code::not_found, "font face missing");
  alias.cached_face = face->second.get();
  face->second->aliased = true;
  font_aliases_[alias.alias_id] = alias;
  font_measure_cache_.push_back(face->second.get());
  return Status::success();
}

Status AssetStore::evict_font(WireReader& payload, Epoch epoch) {
  std::uint32_t family = 0;
  std::uint32_t before_epoch = 0;
  std::uint32_t selection = 0;
  if (!payload.read_u32(family) || !payload.read_u32(before_epoch) ||
      !payload.read_u32(selection) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "font eviction payload is invalid");
  }
  for (auto it = fonts_.begin(); it != fonts_.end();) {
    FontFace& face = *it->second;
    const bool selected = face.family == family && face.last_used <= before_epoch &&
                          ((mix32(face.id ^ selection) + epoch) & 3u) == 1u;
    if (selected) {
      it = fonts_.erase(it);
    } else {
      ++it;
    }
  }
  return Status::success();
}

Status AssetStore::measure_font(WireReader& payload) {
  std::uint32_t alias_id = 0;
  std::uint32_t glyph_seed = 0;
  std::uint32_t expected_cache = 0;
  if (!payload.read_u32(alias_id) || !payload.read_u32(glyph_seed) ||
      !payload.read_u32(expected_cache) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "font measurement payload is invalid");
  }
  auto alias = font_aliases_.find(alias_id);
  if (alias == font_aliases_.end() || expected_cache != font_measure_cache_.size()) {
    return Status::failure(Status::Code::conflict, "font measurement cache changed");
  }
  FontFace* face = alias->second.cached_face;
  if (face == nullptr || face->advances.empty()) {
    return Status::failure(Status::Code::not_found, "font alias is unresolved");
  }
  std::int64_t width = 0;
  for (unsigned i = 0; i < 16; ++i) {
    const std::size_t glyph = (glyph_seed + i * 17u + face->family) % face->advances.size();
    width += face->advances[glyph];
  }
  face->last_used += static_cast<Epoch>((width ^ glyph_seed) & 7u);
  observation_hash_ ^= mix64(static_cast<std::uint64_t>(width) ^ face->id);
  return Status::success();
}

void AssetStore::begin_transaction(Revision revision) { active_revision_ = revision; }

void AssetStore::commit_transaction(Revision revision) {
  if (active_revision_ == revision) {
    rollback_aliases_.erase(
        std::remove_if(rollback_aliases_.begin(), rollback_aliases_.end(),
                       [](Alias* alias) { return alias == nullptr || !alias->rollback_candidate; }),
        rollback_aliases_.end());
  }
  active_revision_ = 0;
}

void AssetStore::abort_transaction(Revision revision) {
  if (active_revision_ == revision) {
    for (Asset* asset : promotion_callbacks_) {
      if (asset != nullptr) asset->staged = false;
    }
    promotion_callbacks_.clear();
  }
  active_revision_ = 0;
}

void AssetStore::reset() {
  assets_.clear();
  aliases_.clear();
  dictionaries_.clear();
  delta_jobs_.clear();
  fonts_.clear();
  font_aliases_.clear();
  promotion_callbacks_.clear();
  rollback_aliases_.clear();
  font_measure_cache_.clear();
  active_revision_ = 0;
  observation_hash_ = 0;
}

std::size_t AssetStore::asset_count() const { return assets_.size(); }
std::size_t AssetStore::dictionary_count() const { return dictionaries_.size(); }
std::uint64_t AssetStore::observation_hash() const { return observation_hash_; }

}  // namespace lanternstage

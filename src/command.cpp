#include "lanternstage/command.h"
#include "lanternstage/wire.h"

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>

namespace lanternstage {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{{'L', 'S', 'T', 'G'}};
constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kRecordHeaderSize = 12;
constexpr std::size_t kMaximumCommands = 4096;
constexpr std::size_t kMaximumPayload = 65535;

bool magic_matches(ByteSpan bytes) {
  return bytes.size >= kMagic.size() &&
         std::equal(kMagic.begin(), kMagic.end(), bytes.data);
}

bool is_transaction_opcode(Opcode op) {
  return op == Opcode::begin_tx || op == Opcode::commit_tx ||
         op == Opcode::abort_tx;
}

}  // namespace

const char* opcode_name(Opcode opcode) {
  switch (opcode) {
    case Opcode::nop: return "nop";
    case Opcode::begin_tx: return "begin_tx";
    case Opcode::commit_tx: return "commit_tx";
    case Opcode::abort_tx: return "abort_tx";
    case Opcode::advance_clock: return "advance_clock";
    case Opcode::asset_define: return "asset_define";
    case Opcode::asset_stage_variant: return "asset_stage_variant";
    case Opcode::asset_drop_unreferenced: return "asset_drop_unreferenced";
    case Opcode::asset_publish: return "asset_publish";
    case Opcode::asset_alias: return "asset_alias";
    case Opcode::asset_replace: return "asset_replace";
    case Opcode::asset_rollback_aliases: return "asset_rollback_aliases";
    case Opcode::asset_resolve_alias: return "asset_resolve_alias";
    case Opcode::dictionary_install: return "dictionary_install";
    case Opcode::dictionary_rotate: return "dictionary_rotate";
    case Opcode::asset_delta_stage: return "asset_delta_stage";
    case Opcode::asset_delta_materialize: return "asset_delta_materialize";
    case Opcode::font_register: return "font_register";
    case Opcode::font_alias: return "font_alias";
    case Opcode::font_evict: return "font_evict";
    case Opcode::font_measure: return "font_measure";
    case Opcode::playlist_create: return "playlist_create";
    case Opcode::playlist_add_item: return "playlist_add_item";
    case Opcode::playlist_link: return "playlist_link";
    case Opcode::playlist_stage_remove: return "playlist_stage_remove";
    case Opcode::playlist_compact: return "playlist_compact";
    case Opcode::playlist_rollback: return "playlist_rollback";
    case Opcode::playlist_preview: return "playlist_preview";
    case Opcode::overlay_create: return "overlay_create";
    case Opcode::overlay_attach: return "overlay_attach";
    case Opcode::overlay_detach: return "overlay_detach";
    case Opcode::overlay_reconcile: return "overlay_reconcile";
    case Opcode::trigger_define: return "trigger_define";
    case Opcode::trigger_clone: return "trigger_clone";
    case Opcode::trigger_retype: return "trigger_retype";
    case Opcode::trigger_fire: return "trigger_fire";
    case Opcode::schedule_define_zone: return "schedule_define_zone";
    case Opcode::schedule_add_rule: return "schedule_add_rule";
    case Opcode::schedule_compile: return "schedule_compile";
    case Opcode::schedule_replace_zone: return "schedule_replace_zone";
    case Opcode::schedule_prune: return "schedule_prune";
    case Opcode::schedule_next: return "schedule_next";
    case Opcode::interval_add: return "interval_add";
    case Opcode::interval_stage_merge: return "interval_stage_merge";
    case Opcode::interval_resize_domain: return "interval_resize_domain";
    case Opcode::interval_commit_merge: return "interval_commit_merge";
    case Opcode::occurrence_pin: return "occurrence_pin";
    case Opcode::occurrence_rebuild: return "occurrence_rebuild";
    case Opcode::occurrence_read: return "occurrence_read";
    case Opcode::locale_add: return "locale_add";
    case Opcode::locale_fallback: return "locale_fallback";
    case Opcode::locale_compile: return "locale_compile";
    case Opcode::locale_compact: return "locale_compact";
    case Opcode::locale_lookup: return "locale_lookup";
    case Opcode::catalog_snapshot: return "catalog_snapshot";
    case Opcode::catalog_mutate: return "catalog_mutate";
    case Opcode::catalog_restore: return "catalog_restore";
    case Opcode::catalog_lookup_snapshot: return "catalog_lookup_snapshot";
    case Opcode::device_add: return "device_add";
    case Opcode::device_lease: return "device_lease";
    case Opcode::device_merge_tenant: return "device_merge_tenant";
    case Opcode::device_expire: return "device_expire";
    case Opcode::device_notify: return "device_notify";
    case Opcode::reservation_open: return "reservation_open";
    case Opcode::reservation_cancel: return "reservation_cancel";
    case Opcode::reservation_expire: return "reservation_expire";
    case Opcode::reservation_sweep: return "reservation_sweep";
    case Opcode::journal_append: return "journal_append";
    case Opcode::journal_cursor: return "journal_cursor";
    case Opcode::journal_compact: return "journal_compact";
    case Opcode::journal_read_cursor: return "journal_read_cursor";
    case Opcode::snapshot_fork: return "snapshot_fork";
    case Opcode::snapshot_intern: return "snapshot_intern";
    case Opcode::snapshot_merge: return "snapshot_merge";
    case Opcode::snapshot_read: return "snapshot_read";
    case Opcode::token_begin: return "token_begin";
    case Opcode::token_checkpoint: return "token_checkpoint";
    case Opcode::token_rollback: return "token_rollback";
    case Opcode::token_release: return "token_release";
    case Opcode::expr_schema: return "expr_schema";
    case Opcode::expr_compile: return "expr_compile";
    case Opcode::expr_migrate_schema: return "expr_migrate_schema";
    case Opcode::expr_eval: return "expr_eval";
    case Opcode::expr_cache_pin: return "expr_cache_pin";
    case Opcode::expr_cache_evict: return "expr_cache_evict";
    case Opcode::expr_cache_eval: return "expr_cache_eval";
    case Opcode::plane_create: return "plane_create";
    case Opcode::plane_palette: return "plane_palette";
    case Opcode::plane_crop: return "plane_crop";
    case Opcode::plane_patch: return "plane_patch";
    case Opcode::tile_plan: return "tile_plan";
    case Opcode::tile_resize: return "tile_resize";
    case Opcode::tile_retain: return "tile_retain";
    case Opcode::tile_render: return "tile_render";
  }
  return "unknown";
}

Result<CommandBundle> parse_command_bundle(ByteSpan bytes) {
  Result<CommandBundle> result;
  if (bytes.size < kHeaderSize || !magic_matches(bytes)) {
    result.status = Status::failure(Status::Code::malformed, "missing LSTG header");
    return result;
  }

  WireReader reader(bytes);
  ByteSpan magic;
  std::uint16_t count = 0;
  std::uint32_t declared_size = 0;
  std::uint32_t declared_hash = 0;
  if (!reader.read_bytes(4, magic) ||
      !reader.read_u16(result.value.version) ||
      !reader.read_u16(result.value.features) ||
      !reader.read_u32(result.value.stream_id) ||
      !reader.read_u16(count) ||
      !reader.skip(2) ||
      !reader.read_u32(declared_size) ||
      !reader.read_u32(declared_hash)) {
    result.status = Status::failure(Status::Code::malformed, "truncated bundle header");
    return result;
  }
  if (result.value.version != 1 || count > kMaximumCommands) {
    result.status = Status::failure(Status::Code::unsupported, "unsupported bundle version or count");
    return result;
  }
  if (declared_size != bytes.size) {
    result.status = Status::failure(Status::Code::malformed, "bundle length mismatch");
    return result;
  }
  const ByteSpan body = bytes.subspan(kHeaderSize, bytes.size - kHeaderSize);
  if (fnv1a32(body, result.value.stream_id ^ 2166136261u) != declared_hash) {
    result.status = Status::failure(Status::Code::malformed, "bundle checksum mismatch");
    return result;
  }

  result.value.commands.reserve(count);
  std::uint32_t previous_sequence = 0;
  for (std::uint16_t index = 0; index < count; ++index) {
    if (reader.remaining() < kRecordHeaderSize) {
      result.status = Status::failure(Status::Code::malformed, "truncated command header");
      return result;
    }
    std::uint8_t raw_opcode = 0;
    Command command;
    std::uint16_t payload_size = 0;
    std::uint32_t checksum = 0;
    if (!reader.read_u8(raw_opcode) || !reader.read_u8(command.flags) ||
        !reader.read_u16(payload_size) || !reader.read_u32(command.sequence) ||
        !reader.read_u32(checksum)) {
      result.status = Status::failure(Status::Code::malformed, "invalid command header");
      return result;
    }
    if (payload_size > kMaximumPayload || payload_size > reader.remaining()) {
      result.status = Status::failure(Status::Code::malformed, "invalid command payload size");
      return result;
    }
    if (index != 0 && command.sequence <= previous_sequence) {
      result.status = Status::failure(Status::Code::conflict, "command sequences must increase");
      return result;
    }
    previous_sequence = command.sequence;
    ByteSpan payload;
    reader.read_bytes(payload_size, payload);
    const std::uint32_t seed = mix32(result.value.stream_id ^ command.sequence ^
                                     (static_cast<std::uint32_t>(raw_opcode) << 24u) ^
                                     command.flags);
    if (fnv1a32(payload, seed) != checksum) {
      result.status = Status::failure(Status::Code::malformed, "command checksum mismatch");
      return result;
    }
    command.opcode = static_cast<Opcode>(raw_opcode);
    command.payload.assign(payload.data, payload.data + payload.size);
    result.value.commands.push_back(std::move(command));
  }
  if (!reader.empty()) {
    result.status = Status::failure(Status::Code::malformed, "trailing bytes after commands");
    return result;
  }
  result.status = validate_command_order(result.value);
  return result;
}

std::vector<std::uint8_t> encode_command_bundle(const CommandBundle& bundle) {
  WireWriter body;
  for (const Command& command : bundle.commands) {
    body.write_u8(static_cast<std::uint8_t>(command.opcode));
    body.write_u8(command.flags);
    body.write_u16(static_cast<std::uint16_t>(command.payload.size()));
    body.write_u32(command.sequence);
    const ByteSpan payload{command.payload.data(), command.payload.size()};
    const std::uint32_t seed = mix32(bundle.stream_id ^ command.sequence ^
                                     (static_cast<std::uint32_t>(command.opcode) << 24u) ^
                                     command.flags);
    body.write_u32(fnv1a32(payload, seed));
    body.write_bytes(payload);
  }

  WireWriter out;
  out.write_bytes({kMagic.data(), kMagic.size()});
  out.write_u16(bundle.version);
  out.write_u16(bundle.features);
  out.write_u32(bundle.stream_id);
  out.write_u16(static_cast<std::uint16_t>(bundle.commands.size()));
  out.write_u16(0);
  out.write_u32(static_cast<std::uint32_t>(kHeaderSize + body.bytes().size()));
  out.write_u32(fnv1a32({body.bytes().data(), body.bytes().size()},
                        bundle.stream_id ^ 2166136261u));
  out.write_bytes({body.bytes().data(), body.bytes().size()});
  return out.take();
}

Status validate_command_order(const CommandBundle& bundle) {
  bool transaction_open = false;
  std::uint32_t tx_depth = 0;
  std::unordered_set<std::uint32_t> seen_sequences;
  for (const Command& command : bundle.commands) {
    if (!seen_sequences.insert(command.sequence).second) {
      return Status::failure(Status::Code::conflict, "duplicate sequence number");
    }
    if (command.opcode == Opcode::begin_tx) {
      ++tx_depth;
      transaction_open = true;
      if (tx_depth > 8) {
        return Status::failure(Status::Code::rejected, "transaction nesting limit exceeded");
      }
    } else if (command.opcode == Opcode::commit_tx || command.opcode == Opcode::abort_tx) {
      if (tx_depth == 0) {
        return Status::failure(Status::Code::conflict, "transaction terminator without begin");
      }
      --tx_depth;
      transaction_open = tx_depth != 0;
    } else if ((command.flags & 0x80u) != 0 && !transaction_open &&
               !is_transaction_opcode(command.opcode)) {
      return Status::failure(Status::Code::conflict, "transactional command outside transaction");
    }
  }
  if (tx_depth != 0) {
    return Status::failure(Status::Code::conflict, "unterminated transaction");
  }
  return Status::success();
}

}  // namespace lanternstage

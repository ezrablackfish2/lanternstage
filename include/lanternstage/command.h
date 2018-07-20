#pragma once

#include "lanternstage/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lanternstage {

enum class Opcode : std::uint8_t {
  nop = 0x00,
  begin_tx = 0x01,
  commit_tx = 0x02,
  abort_tx = 0x03,
  advance_clock = 0x04,

  asset_define = 0x10,
  asset_stage_variant = 0x11,
  asset_drop_unreferenced = 0x12,
  asset_publish = 0x13,
  asset_alias = 0x14,
  asset_replace = 0x15,
  asset_rollback_aliases = 0x16,
  asset_resolve_alias = 0x17,
  dictionary_install = 0x18,
  dictionary_rotate = 0x19,
  asset_delta_stage = 0x1a,
  asset_delta_materialize = 0x1b,
  font_register = 0x1c,
  font_alias = 0x1d,
  font_evict = 0x1e,
  font_measure = 0x1f,

  playlist_create = 0x20,
  playlist_add_item = 0x21,
  playlist_link = 0x22,
  playlist_stage_remove = 0x23,
  playlist_compact = 0x24,
  playlist_rollback = 0x25,
  playlist_preview = 0x26,
  overlay_create = 0x27,
  overlay_attach = 0x28,
  overlay_detach = 0x29,
  overlay_reconcile = 0x2a,
  trigger_define = 0x2b,
  trigger_clone = 0x2c,
  trigger_retype = 0x2d,
  trigger_fire = 0x2e,

  schedule_define_zone = 0x30,
  schedule_add_rule = 0x31,
  schedule_compile = 0x32,
  schedule_replace_zone = 0x33,
  schedule_prune = 0x34,
  schedule_next = 0x35,
  interval_add = 0x36,
  interval_stage_merge = 0x37,
  interval_resize_domain = 0x38,
  interval_commit_merge = 0x39,
  occurrence_pin = 0x3a,
  occurrence_rebuild = 0x3b,
  occurrence_read = 0x3c,

  locale_add = 0x40,
  locale_fallback = 0x41,
  locale_compile = 0x42,
  locale_compact = 0x43,
  locale_lookup = 0x44,
  catalog_snapshot = 0x45,
  catalog_mutate = 0x46,
  catalog_restore = 0x47,
  catalog_lookup_snapshot = 0x48,

  device_add = 0x50,
  device_lease = 0x51,
  device_merge_tenant = 0x52,
  device_expire = 0x53,
  device_notify = 0x54,
  reservation_open = 0x55,
  reservation_cancel = 0x56,
  reservation_expire = 0x57,
  reservation_sweep = 0x58,

  journal_append = 0x60,
  journal_cursor = 0x61,
  journal_compact = 0x62,
  journal_read_cursor = 0x63,
  snapshot_fork = 0x64,
  snapshot_intern = 0x65,
  snapshot_merge = 0x66,
  snapshot_read = 0x67,
  token_begin = 0x68,
  token_checkpoint = 0x69,
  token_rollback = 0x6a,
  token_release = 0x6b,

  expr_schema = 0x70,
  expr_compile = 0x71,
  expr_migrate_schema = 0x72,
  expr_eval = 0x73,
  expr_cache_pin = 0x74,
  expr_cache_evict = 0x75,
  expr_cache_eval = 0x76,

  plane_create = 0x80,
  plane_palette = 0x81,
  plane_crop = 0x82,
  plane_patch = 0x83,
  tile_plan = 0x84,
  tile_resize = 0x85,
  tile_retain = 0x86,
  tile_render = 0x87,
};

struct Command {
  Opcode opcode{Opcode::nop};
  std::uint8_t flags{0};
  std::uint32_t sequence{0};
  std::vector<std::uint8_t> payload;
};

struct CommandBundle {
  std::uint16_t version{1};
  std::uint16_t features{0};
  std::uint32_t stream_id{0};
  std::vector<Command> commands;
};

const char* opcode_name(Opcode opcode);
Result<CommandBundle> parse_command_bundle(ByteSpan bytes);
std::vector<std::uint8_t> encode_command_bundle(const CommandBundle& bundle);
Status validate_command_order(const CommandBundle& bundle);

}  // namespace lanternstage

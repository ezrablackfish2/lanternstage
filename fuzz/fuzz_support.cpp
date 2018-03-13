#include "fuzz_support.h"

#include "lanternstage/wire.h"

#include <algorithm>
#include <array>
#include <limits>
#include <random>

namespace lanternstage::fuzzing {
namespace {

class Random {
 public:
  explicit Random(unsigned seed) : state_(mix64(seed + 0x9e3779b97f4a7c15ull)) {}

  std::uint32_t next() {
    state_ ^= state_ >> 12u;
    state_ ^= state_ << 25u;
    state_ ^= state_ >> 27u;
    return static_cast<std::uint32_t>((state_ * 2685821657736338717ull) >> 32u);
  }

  std::size_t index(std::size_t size) { return size == 0 ? 0 : next() % size; }
  bool coin(unsigned numerator = 1, unsigned denominator = 2) {
    return denominator != 0 && next() % denominator < numerator;
  }

 private:
  std::uint64_t state_;
};

void write_asset_definition(WireWriter& out, unsigned seed) {
  const std::array<std::uint8_t, 16> pixels{{0, 1, 2, 3, 4, 5, 6, 7,
                                             8, 9, 10, 11, 12, 13, 14, 15}};
  out.write_u32(100 + seed % 31);
  out.write_u32(1 + seed % 5);
  out.write_u32(4);
  out.write_u32(4);
  out.write_u16(5);
  out.write_u16(pixels.size());
  out.write_string("asset");
  out.write_bytes({pixels.data(), pixels.size()});
}

void write_zone(WireWriter& out, unsigned seed) {
  out.write_u32(200 + seed % 17);
  out.write_u32(1 + seed % 3);
  out.write_u8(3);
  out.write_u8(2);
  out.write_string("UTC");
  out.write_u64(1);
  out.write_i32(0);
  out.write_u8(0);
  out.write_u64(2000000000ull);
  out.write_i32(static_cast<std::int32_t>((seed % 3) * 900));
  out.write_u8(seed & 1u);
}

void write_locale(WireWriter& out, unsigned seed) {
  const std::string tag = seed & 1u ? "en-US" : "fr-FR";
  const std::string value = seed & 1u ? "Welcome" : "Bienvenue";
  out.write_u32(300 + seed % 19);
  out.write_u32(1 + seed % 4);
  out.write_u8(static_cast<std::uint8_t>(tag.size()));
  out.write_u16(1);
  out.write_string(tag);
  out.write_u32(1);
  out.write_u32(1);
  out.write_u16(static_cast<std::uint16_t>(value.size()));
  out.write_string(value);
}

void write_device(WireWriter& out, unsigned seed) {
  const std::string serial = "LST-" + std::to_string(seed % 10000);
  out.write_u32(400 + seed % 23);
  out.write_u32(1 + seed % 4);
  out.write_u32(1 + seed % 7);
  out.write_u32(0x0000000fu | ((seed & 0xffu) << 8u));
  out.write_u8(static_cast<std::uint8_t>(serial.size()));
  out.write_u8(1);
  out.write_string(serial);
}

void write_expression_schema(WireWriter& out, unsigned seed) {
  out.write_u32(500 + seed % 29);
  out.write_u32(1 + seed % 3);
  out.write_u16(3);
  out.write_u32(1);
  out.write_u8(1);
  out.write_u8(8);
  out.write_string("audience");
  out.write_u32(2);
  out.write_u8(1);
  out.write_u8(6);
  out.write_string("hour24");
  out.write_u32(3);
  out.write_u8(2);
  out.write_u8(6);
  out.write_string("online");
}

void write_plane(WireWriter& out, unsigned seed) {
  constexpr std::uint32_t width = 8;
  constexpr std::uint32_t height = 8;
  constexpr std::uint32_t stride = 8;
  out.write_u32(600 + seed % 31);
  out.write_u32(1 + seed % 3);
  out.write_u32(width);
  out.write_u32(height);
  out.write_u32(stride);
  out.write_u16(4);
  out.write_u32(stride * height);
  out.write_u32(0xff000000u);
  out.write_u32(0xffffffffu);
  out.write_u32(0xffff0000u);
  out.write_u32(0xff00ff00u);
  for (std::uint32_t i = 0; i < stride * height; ++i) out.write_u8((i + seed) & 3u);
}

std::uint8_t random_opcode_for_mode(Random& random, ReplayMode mode) {
  const std::vector<Opcode> opcodes = opcodes_for_mode(mode);
  return static_cast<std::uint8_t>(opcodes[random.index(opcodes.size())]);
}

Command make_command(Opcode opcode, std::uint32_t sequence, unsigned seed) {
  Command command;
  command.opcode = opcode;
  command.sequence = sequence;
  command.flags = static_cast<std::uint8_t>(seed & 0x3fu);
  command.payload = default_payload(opcode, seed);
  return command;
}

std::size_t encoded_size(const CommandBundle& bundle) {
  std::size_t size = 24;
  for (const Command& command : bundle.commands) size += 12 + command.payload.size();
  return size;
}

}  // namespace

std::vector<Opcode> opcodes_for_mode(ReplayMode mode) {
  std::vector<Opcode> opcodes{Opcode::nop, Opcode::advance_clock};
  auto append = [&](std::uint8_t first, std::uint8_t last) {
    for (std::uint16_t raw = first; raw <= last; ++raw) {
      const Opcode opcode = static_cast<Opcode>(raw);
      if (std::string(opcode_name(opcode)) != "unknown") opcodes.push_back(opcode);
    }
  };
  switch (mode) {
    case ReplayMode::full:
      append(0x10, 0x87);
      break;
    case ReplayMode::package:
      append(0x10, 0x48);
      break;
    case ReplayMode::snapshot:
      append(0x40, 0x6b);
      break;
    case ReplayMode::compositor:
      append(0x10, 0x1f);
      append(0x80, 0x87);
      break;
    case ReplayMode::expression:
      append(0x40, 0x48);
      append(0x70, 0x76);
      break;
  }
  return opcodes;
}

std::vector<std::uint8_t> default_payload(Opcode opcode, unsigned seed) {
  WireWriter out;
  switch (opcode) {
    case Opcode::nop:
      break;
    case Opcode::begin_tx:
      out.write_u32(10 + seed % 1000);
      out.write_u32(mix32(seed));
      out.write_u32(0);
      break;
    case Opcode::commit_tx:
      out.write_u32(10 + seed % 1000);
      out.write_u32(mix32(seed));
      out.write_u32(seed % 8);
      break;
    case Opcode::abort_tx:
      out.write_u32(10 + seed % 1000);
      out.write_u32(mix32(seed));
      out.write_u32(seed ^ 0x55aa55aau);
      break;
    case Opcode::advance_clock:
      out.write_u64(1 + seed % 86400);
      out.write_u32(seed % 8);
      out.write_u32(mix32(seed ^ 77u));
      break;
    case Opcode::asset_define:
      write_asset_definition(out, seed);
      break;
    case Opcode::asset_stage_variant:
      out.write_u32(100 + seed % 31);
      out.write_u32(seed % 8);
      out.write_u32(seed % 4);
      break;
    case Opcode::asset_drop_unreferenced:
      out.write_u32(1 + seed % 20);
      out.write_u32(mix32(seed));
      break;
    case Opcode::asset_publish:
      out.write_u32(seed % 8);
      out.write_u32(mix32(seed ^ 17u));
      break;
    case Opcode::asset_alias:
      out.write_u32(700 + seed % 31);
      out.write_u32(100 + seed % 31);
      out.write_u32(1 + seed % 5);
      out.write_u8(seed & 1u);
      break;
    case Opcode::asset_replace: {
      const std::array<std::uint8_t, 8> bytes{{1, 3, 5, 7, 9, 11, 13, 15}};
      out.write_u32(100 + seed % 31);
      out.write_u32(2 + seed % 9);
      out.write_u16(bytes.size());
      out.write_u16(7);
      out.write_bytes({bytes.data(), bytes.size()});
      out.write_string("replace");
      break;
    }
    case Opcode::asset_rollback_aliases:
      out.write_u32(1 + seed % 16);
      out.write_u32(mix32(seed));
      break;
    case Opcode::asset_resolve_alias:
      out.write_u32(700 + seed % 31);
      out.write_u32(seed);
      break;
    case Opcode::dictionary_install:
      out.write_u32(800 + seed % 17);
      out.write_u32(seed % 8);
      out.write_u16(32);
      for (unsigned i = 0; i < 32; ++i) out.write_u8(mix32(seed + i));
      break;
    case Opcode::dictionary_rotate:
      out.write_u32(800 + seed % 17);
      out.write_u32(800 + (seed + 1) % 17);
      out.write_u32(seed % 8);
      break;
    case Opcode::asset_delta_stage:
      out.write_u32(900 + seed % 31);
      out.write_u32(100 + seed % 31);
      out.write_u32(800 + seed % 17);
      out.write_u32(16 + seed % 256);
      out.write_u16(12);
      for (unsigned i = 0; i < 6; ++i) {
        out.write_u8(1 + (seed + i) % 8);
        out.write_u8((seed * 7 + i * 13) & 0xffu);
      }
      break;
    case Opcode::asset_delta_materialize:
      out.write_u32(900 + seed % 31);
      out.write_u32(950 + seed % 31);
      out.write_u32(mix32(seed));
      break;
    case Opcode::font_register:
      out.write_u32(1000 + seed % 23);
      out.write_u32(seed % 7);
      out.write_u16(16);
      for (unsigned i = 0; i < 16; ++i) out.write_u16(200 + (seed + i) % 500);
      break;
    case Opcode::font_alias:
      out.write_u32(1050 + seed % 23);
      out.write_u32(1000 + seed % 23);
      out.write_u32(seed % 8);
      break;
    case Opcode::font_evict:
      out.write_u32(seed % 7);
      out.write_u32(seed % 8);
      out.write_u32(mix32(seed));
      break;
    case Opcode::font_measure:
      out.write_u32(1050 + seed % 23);
      out.write_u32(mix32(seed));
      out.write_u32(1 + seed % 8);
      break;
    case Opcode::playlist_create:
      out.write_u32(1100 + seed % 17);
      out.write_u32(1 + seed % 5);
      out.write_u32(1 + seed % 7);
      break;
    case Opcode::playlist_add_item:
      out.write_u32(1100 + seed % 17);
      out.write_u32(1 + seed % 31);
      out.write_u32(100 + seed % 31);
      out.write_u32(1000 + seed % 100000);
      out.write_u32(1 + seed % 5);
      out.write_u16(4);
      out.write_string("item");
      break;
    case Opcode::playlist_link:
      out.write_u32(1100 + seed % 17);
      out.write_u32(1 + seed % 31);
      out.write_u32(1 + (seed + 1) % 31);
      out.write_u32(seed % 31);
      break;
    case Opcode::playlist_stage_remove:
      out.write_u32(1100 + seed % 17);
      out.write_u32(1 + seed % 31);
      out.write_u32(seed % 31);
      break;
    case Opcode::playlist_compact:
      out.write_u32(1100 + seed % 17);
      out.write_u32(1 + seed % 5);
      out.write_u32(mix32(seed));
      break;
    case Opcode::playlist_rollback:
      out.write_u32(1 + seed % 8);
      out.write_u32(mix32(seed));
      break;
    case Opcode::playlist_preview:
      out.write_u32(1100 + seed % 17);
      out.write_u32(1 + seed % 32);
      out.write_u32(mix32(seed));
      break;
    case Opcode::overlay_create:
      out.write_u32(1200 + seed % 23);
      out.write_u32(1100 + seed % 17);
      out.write_u32(seed % 16);
      out.write_u16(8);
      for (unsigned i = 0; i < 8; ++i) out.write_u8(seed + i);
      break;
    case Opcode::overlay_attach:
      out.write_u32(1200 + seed % 23);
      out.write_u32(seed % 11);
      out.write_u32(1 + seed % 7);
      out.write_u8(1 + seed % 3);
      break;
    case Opcode::overlay_detach:
      out.write_u32(1200 + seed % 23);
      out.write_u32(seed % 11);
      out.write_u32(1 + seed % 3);
      break;
    case Opcode::overlay_reconcile:
      out.write_u32(seed % 11);
      out.write_u32(1 + seed % 5);
      out.write_u32(mix32(seed));
      break;
    case Opcode::schedule_define_zone:
      write_zone(out, seed);
      break;
    case Opcode::locale_add:
      write_locale(out, seed);
      break;
    case Opcode::device_add:
      write_device(out, seed);
      break;
    case Opcode::expr_schema:
      write_expression_schema(out, seed);
      break;
    case Opcode::plane_create:
      write_plane(out, seed);
      break;
    default:
      for (unsigned i = 0; i < 12 + seed % 20; ++i) out.write_u8(mix32(seed + i));
      break;
  }
  return out.take();
}

CommandBundle make_seed_bundle(ReplayMode mode, std::uint32_t stream_id) {
  CommandBundle bundle;
  bundle.stream_id = stream_id ^ static_cast<std::uint32_t>(mode);
  std::uint32_t sequence = 10;
  auto add = [&](Opcode opcode, unsigned seed, std::uint8_t flags = 0) {
    Command command = make_command(opcode, sequence, seed);
    command.flags = flags;
    bundle.commands.push_back(std::move(command));
    sequence += 10;
  };
  add(Opcode::asset_define, 1, 0x10);
  add(Opcode::asset_define, 2, 0x20);
  if (mode == ReplayMode::full || mode == ReplayMode::package) {
    add(Opcode::playlist_create, 3);
    add(Opcode::playlist_add_item, 4);
    add(Opcode::schedule_define_zone, 5);
    add(Opcode::locale_add, 6);
  }
  if (mode == ReplayMode::full || mode == ReplayMode::snapshot) {
    add(Opcode::locale_add, 7);
    add(Opcode::device_add, 8);
  }
  if (mode == ReplayMode::full || mode == ReplayMode::expression) {
    add(Opcode::expr_schema, 9);
  }
  if (mode == ReplayMode::full || mode == ReplayMode::compositor) {
    add(Opcode::plane_create, 10);
  }
  return bundle;
}

void normalize_sequences(CommandBundle& bundle) {
  std::uint32_t sequence = 10;
  for (Command& command : bundle.commands) {
    command.sequence = sequence;
    if (sequence > std::numeric_limits<std::uint32_t>::max() - 10) break;
    sequence += 10;
  }
}

void repair_transactions(CommandBundle& bundle, unsigned seed) {
  std::vector<Command> repaired;
  repaired.reserve(bundle.commands.size() + 4);
  std::vector<std::pair<Revision, std::uint32_t>> stack;
  std::uint32_t revision = 100 + seed % 1000;
  for (Command command : bundle.commands) {
    if (command.opcode == Opcode::begin_tx) {
      WireWriter payload;
      ++revision;
      const std::uint32_t cookie = mix32(seed ^ revision ^ command.sequence);
      payload.write_u32(revision);
      payload.write_u32(cookie);
      payload.write_u32(stack.empty() ? 0 : stack.back().first);
      command.payload = payload.take();
      stack.push_back({revision, cookie});
      repaired.push_back(std::move(command));
      continue;
    }
    if (command.opcode == Opcode::commit_tx || command.opcode == Opcode::abort_tx) {
      if (stack.empty()) continue;
      const auto [active_revision, cookie] = stack.back();
      stack.pop_back();
      WireWriter payload;
      payload.write_u32(active_revision);
      payload.write_u32(cookie);
      if (command.opcode == Opcode::commit_tx) payload.write_u32(0);
      else payload.write_u32((active_revision ^ cookie ^ bundle.stream_id) & ~1u |
                             (active_revision & 1u));
      command.payload = payload.take();
      repaired.push_back(std::move(command));
      continue;
    }
    repaired.push_back(std::move(command));
  }
  while (!stack.empty()) {
    const auto [active_revision, cookie] = stack.back();
    stack.pop_back();
    WireWriter payload;
    payload.write_u32(active_revision);
    payload.write_u32(cookie);
    payload.write_u32(0);
    Command command;
    command.opcode = Opcode::commit_tx;
    command.payload = payload.take();
    repaired.push_back(std::move(command));
  }
  bundle.commands = std::move(repaired);
  normalize_sequences(bundle);
}

std::size_t mutate_bundle(std::uint8_t* data, std::size_t size, std::size_t maximum_size,
                          unsigned seed, ReplayMode mode, MutateFunction mutate) {
  Random random(seed);
  Result<CommandBundle> parsed = parse_command_bundle({data, size});
  CommandBundle bundle = parsed ? std::move(parsed.value) : make_seed_bundle(mode, seed);
  bundle.stream_id ^= mix32(seed + random.next());
  const unsigned action = random.next() % 8u;
  if (bundle.commands.empty()) bundle = make_seed_bundle(mode, seed);
  if (action == 0 && bundle.commands.size() < 256) {
    const auto opcode = static_cast<Opcode>(random_opcode_for_mode(random, mode));
    Command command = make_command(opcode, 0, seed ^ random.next());
    bundle.commands.insert(bundle.commands.begin() + static_cast<std::ptrdiff_t>(random.index(bundle.commands.size() + 1)),
                           std::move(command));
  } else if (action == 1 && bundle.commands.size() > 1) {
    bundle.commands.erase(bundle.commands.begin() + static_cast<std::ptrdiff_t>(random.index(bundle.commands.size())));
  } else if (action == 2 && bundle.commands.size() < 256) {
    Command copy = bundle.commands[random.index(bundle.commands.size())];
    copy.flags ^= static_cast<std::uint8_t>(random.next());
    bundle.commands.insert(bundle.commands.begin() + static_cast<std::ptrdiff_t>(random.index(bundle.commands.size() + 1)),
                           std::move(copy));
  } else if (action == 3 && bundle.commands.size() > 1) {
    const std::size_t a = random.index(bundle.commands.size());
    const std::size_t b = random.index(bundle.commands.size());
    std::swap(bundle.commands[a], bundle.commands[b]);
  } else if (action == 4) {
    Command& command = bundle.commands[random.index(bundle.commands.size())];
    command.opcode = static_cast<Opcode>(random_opcode_for_mode(random, mode));
    command.payload = default_payload(command.opcode, seed ^ random.next());
  } else if (action == 5) {
    Command& command = bundle.commands[random.index(bundle.commands.size())];
    const std::size_t current = command.payload.size();
    const std::size_t limit = std::min<std::size_t>(65535, maximum_size / 2);
    command.payload.resize(std::max<std::size_t>(1, std::min(current + 64, limit)));
    const std::size_t mutated = mutate(command.payload.data(), current, command.payload.size());
    command.payload.resize(mutated);
  } else if (action == 6) {
    Command& command = bundle.commands[random.index(bundle.commands.size())];
    command.flags ^= static_cast<std::uint8_t>(1u << (random.next() & 7u));
  } else {
    bundle.features ^= static_cast<std::uint16_t>(random.next());
  }
  if (random.coin(1, 3)) repair_transactions(bundle, seed);
  else normalize_sequences(bundle);
  while (!bundle.commands.empty() && encoded_size(bundle) > maximum_size) bundle.commands.pop_back();
  std::vector<std::uint8_t> encoded = encode_command_bundle(bundle);
  if (encoded.size() > maximum_size) return size;
  std::copy(encoded.begin(), encoded.end(), data);
  return encoded.size();
}

}  // namespace lanternstage::fuzzing

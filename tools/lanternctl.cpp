#include "lanternstage/command.h"
#include "lanternstage/deployment_plan.h"
#include "lanternstage/diagnostics.h"
#include "lanternstage/engine.h"
#include "lanternstage/manifest.h"
#include "lanternstage/query.h"
#include "lanternstage/snapshot_codec.h"
#include "lanternstage/text.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace lanternstage;

struct Arguments {
  std::vector<std::string> values;

  bool empty() const { return values.empty(); }
  std::size_t size() const { return values.size(); }
  const std::string& operator[](std::size_t index) const { return values[index]; }
};

std::optional<std::string> read_text_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (!input.good() && !input.eof()) return std::nullopt;
  return buffer.str();
}

std::optional<std::vector<std::uint8_t>> read_binary_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  input.seekg(0, std::ios::end);
  const std::streamoff length = input.tellg();
  if (length < 0 || static_cast<std::uint64_t>(length) >
                        std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), length);
  }
  if (!input && !input.eof()) return std::nullopt;
  return bytes;
}

bool write_text_file(const std::string& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(output);
}

bool write_binary_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  return static_cast<bool>(output);
}

template <typename T>
std::optional<T> parse_unsigned(std::string_view text) {
  T value{};
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
  return value;
}

ReplayMode parse_mode(std::string_view text, bool* valid) {
  *valid = true;
  if (text == "full") return ReplayMode::full;
  if (text == "package") return ReplayMode::package;
  if (text == "snapshot") return ReplayMode::snapshot;
  if (text == "compositor") return ReplayMode::compositor;
  if (text == "expression") return ReplayMode::expression;
  *valid = false;
  return ReplayMode::full;
}

NetworkClass parse_network(std::string_view text, bool* valid) {
  *valid = true;
  if (text == "offline") return NetworkClass::offline;
  if (text == "metered") return NetworkClass::metered;
  if (text == "standard") return NetworkClass::standard;
  if (text == "high" || text == "high-bandwidth") return NetworkClass::high_bandwidth;
  *valid = false;
  return NetworkClass::standard;
}

std::vector<std::string> split_tsv(const std::string& line) {
  std::vector<std::string> values;
  std::size_t begin = 0;
  while (begin <= line.size()) {
    const std::size_t end = line.find('\t', begin);
    if (end == std::string::npos) {
      values.push_back(line.substr(begin));
      break;
    }
    values.push_back(line.substr(begin, end - begin));
    begin = end + 1;
  }
  return values;
}

Result<std::vector<PlanningDevice>> read_devices(const std::string& path) {
  Result<std::vector<PlanningDevice>> result;
  std::ifstream input(path);
  if (!input) {
    result.status = Status::failure(Status::Code::not_found,
                                    "unable to open device inventory");
    return result;
  }

  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> fields = split_tsv(line);
    if (fields.size() < 9) {
      result.status = Status::failure(Status::Code::malformed,
          "device row " + std::to_string(line_number) + " has fewer than nine fields");
      return result;
    }

    auto id = parse_unsigned<std::uint32_t>(fields[0]);
    auto capabilities = parse_unsigned<std::uint32_t>(fields[3]);
    auto capacity = parse_unsigned<std::uint64_t>(fields[4]);
    auto available = parse_unsigned<std::uint64_t>(fields[5]);
    auto width = parse_unsigned<std::uint32_t>(fields[6]);
    auto height = parse_unsigned<std::uint32_t>(fields[7]);
    auto bandwidth = parse_unsigned<std::uint32_t>(fields[8]);
    bool network_valid = false;
    NetworkClass network = fields.size() > 9
        ? parse_network(fields[9], &network_valid)
        : NetworkClass::standard;
    if (fields.size() <= 9) network_valid = true;
    if (!id || !capabilities || !capacity || !available || !width || !height ||
        !bandwidth || !network_valid) {
      result.status = Status::failure(Status::Code::malformed,
          "device row " + std::to_string(line_number) + " contains an invalid field");
      return result;
    }

    PlanningDevice device;
    device.id = *id;
    device.name = fields[1];
    device.locale = fields[2];
    device.capabilities = *capabilities;
    device.storage_capacity = *capacity;
    device.storage_available = *available;
    device.viewport_width = *width;
    device.viewport_height = *height;
    device.bandwidth_kib_per_second = *bandwidth;
    device.network = network;
    if (fields.size() > 10) {
      auto from = parse_unsigned<Tick>(fields[10]);
      if (!from) {
        result.status = Status::failure(Status::Code::malformed,
            "device row " + std::to_string(line_number) + " has an invalid start time");
        return result;
      }
      device.available_from = *from;
    }
    if (fields.size() > 11) {
      auto until = parse_unsigned<Tick>(fields[11]);
      if (!until) {
        result.status = Status::failure(Status::Code::malformed,
            "device row " + std::to_string(line_number) + " has an invalid end time");
        return result;
      }
      device.available_until = *until;
    }
    result.value.push_back(std::move(device));
  }
  result.status = Status::success();
  return result;
}

void print_diagnostics(const DiagnosticSink& diagnostics) {
  for (const Diagnostic& diagnostic : diagnostics.diagnostics()) {
    std::cerr << format_diagnostic(diagnostic) << '\n';
  }
}

void print_usage(std::ostream& out) {
  out << "LanternStage package and replay utility\n\n"
      << "Usage:\n"
      << "  lanternctl validate <manifest.lst>\n"
      << "  lanternctl normalize <manifest.lst> [output.lst]\n"
      << "  lanternctl digest <manifest.lst>\n"
      << "  lanternctl replay <stream.lstg> [full|package|snapshot|compositor|expression]\n"
      << "  lanternctl snapshot-info <snapshot.lss>\n"
      << "  lanternctl snapshot-normalize <input.lss> <output.lss>\n"
      << "  lanternctl query <expression> [name=value ...]\n"
      << "  lanternctl plan <manifest.lst> <devices.tsv> <now> [activation] [output.json]\n\n"
      << "Device TSV columns:\n"
      << "  id  name  locale  capabilities  capacity  available  width  height  bandwidth_kib_s\n"
      << "  [network] [available_from] [available_until]\n";
}

int command_validate(const Arguments& args) {
  if (args.size() != 2) {
    std::cerr << "validate requires a manifest path\n";
    return 2;
  }
  auto text = read_text_file(args[1]);
  if (!text) {
    std::cerr << "unable to read " << args[1] << '\n';
    return 1;
  }
  DiagnosticSink diagnostics;
  Result<PackageManifest> parsed = parse_manifest_text(*text, &diagnostics, args[1]);
  if (parsed) validate_manifest(parsed.value, &diagnostics);
  print_diagnostics(diagnostics);
  if (!parsed || diagnostics.has_errors()) return 1;
  std::cout << "valid package " << parsed.value.package_id
            << " revision " << parsed.value.revision
            << ": " << parsed.value.assets.size() << " assets, "
            << parsed.value.playlists.size() << " playlists, "
            << parsed.value.device_groups.size() << " device groups\n";
  return 0;
}

int command_normalize(const Arguments& args) {
  if (args.size() < 2 || args.size() > 3) {
    std::cerr << "normalize requires an input and optional output path\n";
    return 2;
  }
  auto text = read_text_file(args[1]);
  if (!text) {
    std::cerr << "unable to read " << args[1] << '\n';
    return 1;
  }
  DiagnosticSink diagnostics;
  Result<PackageManifest> parsed = parse_manifest_text(*text, &diagnostics, args[1]);
  if (!parsed || !validate_manifest(parsed.value, &diagnostics)) {
    print_diagnostics(diagnostics);
    return 1;
  }
  const std::string normalized = serialize_manifest_text(parsed.value);
  if (args.size() == 3) {
    if (!write_text_file(args[2], normalized)) {
      std::cerr << "unable to write " << args[2] << '\n';
      return 1;
    }
  } else {
    std::cout << normalized;
  }
  return 0;
}

int command_digest(const Arguments& args) {
  if (args.size() != 2) {
    std::cerr << "digest requires a manifest path\n";
    return 2;
  }
  auto text = read_text_file(args[1]);
  if (!text) return 1;
  DiagnosticSink diagnostics;
  Result<PackageManifest> parsed = parse_manifest_text(*text, &diagnostics, args[1]);
  if (!parsed || !validate_manifest(parsed.value, &diagnostics)) {
    print_diagnostics(diagnostics);
    return 1;
  }
  std::cout << std::hex << manifest_digest(parsed.value) << std::dec << '\n';
  return 0;
}

int command_replay(const Arguments& args) {
  if (args.size() < 2 || args.size() > 3) {
    std::cerr << "replay requires a stream path and optional mode\n";
    return 2;
  }
  auto bytes = read_binary_file(args[1]);
  if (!bytes) {
    std::cerr << "unable to read " << args[1] << '\n';
    return 1;
  }
  bool valid_mode = true;
  ReplayMode mode = args.size() == 3 ? parse_mode(args[2], &valid_mode) : ReplayMode::full;
  if (!valid_mode) {
    std::cerr << "unknown replay mode\n";
    return 2;
  }
  PlaybackEngine engine;
  Result<ReplayStats> replay = engine.replay({bytes->data(), bytes->size()}, mode);
  if (!replay) {
    std::cerr << replay.status.message << '\n';
    return 1;
  }
  const ReplayStats& stats = replay.value;
  std::cout << "commands_seen=" << stats.commands_seen << '\n'
            << "commands_applied=" << stats.commands_applied << '\n'
            << "commands_rejected=" << stats.commands_rejected << '\n'
            << "transactions_committed=" << stats.transactions_committed << '\n'
            << "transactions_aborted=" << stats.transactions_aborted << '\n'
            << "final_clock=" << stats.final_clock << '\n'
            << "final_revision=" << stats.final_revision << '\n'
            << "observation_hash=" << std::hex << stats.observation_hash << std::dec << '\n';
  return 0;
}

int command_snapshot_info(const Arguments& args) {
  if (args.size() != 2) {
    std::cerr << "snapshot-info requires a snapshot path\n";
    return 2;
  }
  auto bytes = read_binary_file(args[1]);
  if (!bytes) return 1;
  Result<EngineSnapshot> snapshot = decode_snapshot({bytes->data(), bytes->size()});
  if (!snapshot) {
    std::cerr << snapshot.status.message << '\n';
    return 1;
  }
  std::uint64_t payload_bytes = 0;
  for (const SnapshotChunk& chunk : snapshot.value.chunks) payload_bytes += chunk.bytes.size();
  std::cout << "version=" << snapshot.value.version.major << '.'
            << snapshot.value.version.minor << '.' << snapshot.value.version.patch << '\n'
            << "stream_id=" << snapshot.value.stream_id << '\n'
            << "revision=" << snapshot.value.revision << '\n'
            << "clock=" << snapshot.value.clock << '\n'
            << "chunks=" << snapshot.value.chunks.size() << '\n'
            << "payload_bytes=" << payload_bytes << '\n'
            << "metadata_entries=" << snapshot.value.metadata.size() << '\n'
            << "digest=" << std::hex << snapshot_digest(snapshot.value) << std::dec << '\n';
  return 0;
}

int command_snapshot_normalize(const Arguments& args) {
  if (args.size() != 3) {
    std::cerr << "snapshot-normalize requires input and output paths\n";
    return 2;
  }
  auto bytes = read_binary_file(args[1]);
  if (!bytes) return 1;
  Result<EngineSnapshot> snapshot = decode_snapshot({bytes->data(), bytes->size()});
  if (!snapshot) {
    std::cerr << snapshot.status.message << '\n';
    return 1;
  }
  snapshot.value.chunks = coalesce_snapshot_chunks(snapshot.value.chunks);
  std::vector<std::uint8_t> encoded = encode_snapshot(snapshot.value);
  if (!write_binary_file(args[2], encoded)) return 1;
  return 0;
}

QueryValue parse_query_value(const std::string& value) {
  if (value == "true") return true;
  if (value == "false") return false;
  if (value == "null") return std::monostate{};
  std::int64_t integer = 0;
  auto parsed = std::from_chars(value.data(), value.data() + value.size(), integer);
  if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) return integer;
  return value;
}

int command_query(const Arguments& args) {
  if (args.size() < 2) {
    std::cerr << "query requires an expression\n";
    return 2;
  }
  DiagnosticSink diagnostics;
  Result<QueryExpression> expression = parse_query(args[1], &diagnostics);
  if (!expression) {
    print_diagnostics(diagnostics);
    return 1;
  }
  QueryRow row;
  for (std::size_t i = 2; i < args.size(); ++i) {
    const std::size_t equals = args[i].find('=');
    if (equals == std::string::npos || equals == 0) {
      std::cerr << "row values must use name=value syntax\n";
      return 2;
    }
    row[args[i].substr(0, equals)] = parse_query_value(args[i].substr(equals + 1));
  }
  std::cout << (expression.value.evaluate(row) ? "true" : "false") << '\n';
  return 0;
}

int command_plan(const Arguments& args) {
  if (args.size() < 4 || args.size() > 6) {
    std::cerr << "plan requires manifest, devices, now, optional activation and output\n";
    return 2;
  }
  auto text = read_text_file(args[1]);
  auto now = parse_unsigned<Tick>(args[3]);
  if (!text || !now) {
    std::cerr << "unable to read manifest or parse current time\n";
    return 1;
  }
  Tick activation = 0;
  if (args.size() >= 5) {
    auto parsed = parse_unsigned<Tick>(args[4]);
    if (!parsed) {
      std::cerr << "invalid activation time\n";
      return 2;
    }
    activation = *parsed;
  }
  DiagnosticSink diagnostics;
  Result<PackageManifest> manifest = parse_manifest_text(*text, &diagnostics, args[1]);
  if (!manifest || !validate_manifest(manifest.value, &diagnostics)) {
    print_diagnostics(diagnostics);
    return 1;
  }
  Result<std::vector<PlanningDevice>> devices = read_devices(args[2]);
  if (!devices) {
    std::cerr << devices.status.message << '\n';
    return 1;
  }
  DeploymentPlanner planner;
  Result<DeploymentPlan> plan = planner.create_plan(manifest.value, devices.value, *now, activation);
  const std::string serialized = serialize_deployment_plan(plan.value);
  if (args.size() == 6) {
    if (!write_text_file(args[5], serialized)) return 1;
  } else {
    std::cout << serialized;
  }
  return plan && plan.value.feasible() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Arguments args;
  for (int i = 1; i < argc; ++i) args.values.emplace_back(argv[i]);
  if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
    print_usage(args.empty() ? std::cerr : std::cout);
    return args.empty() ? 2 : 0;
  }
  if (args[0] == "validate") return command_validate(args);
  if (args[0] == "normalize") return command_normalize(args);
  if (args[0] == "digest") return command_digest(args);
  if (args[0] == "replay") return command_replay(args);
  if (args[0] == "snapshot-info") return command_snapshot_info(args);
  if (args[0] == "snapshot-normalize") return command_snapshot_normalize(args);
  if (args[0] == "query") return command_query(args);
  if (args[0] == "plan") return command_plan(args);
  std::cerr << "unknown command: " << args[0] << "\n\n";
  print_usage(std::cerr);
  return 2;
}

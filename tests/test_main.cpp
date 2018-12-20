#include "lanternstage/command.h"
#include "lanternstage/deployment_plan.h"
#include "lanternstage/diagnostics.h"
#include "lanternstage/engine.h"
#include "lanternstage/layout.h"
#include "lanternstage/manifest.h"
#include "lanternstage/query.h"
#include "lanternstage/snapshot_codec.h"
#include "lanternstage/text.h"
#include "lanternstage/time_math.h"
#include "lanternstage/wire.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace lanternstage;

struct TestCase {
  const char* name;
  std::function<void()> function;
};

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Register {
  Register(const char* name, std::function<void()> function) {
    registry().push_back({name, std::move(function)});
  }
};

int failures = 0;

void fail(const char* expression, const char* file, int line) {
  std::cerr << file << ':' << line << ": assertion failed: " << expression << '\n';
  ++failures;
}

#define TEST(name) \
  void test_##name(); \
  Register register_##name(#name, test_##name); \
  void test_##name()
#define CHECK(expression) do { if (!(expression)) fail(#expression, __FILE__, __LINE__); } while (false)
#define CHECK_EQ(left, right) do { const auto check_left_value = (left); const auto check_right_value = (right); \
  if (!(check_left_value == check_right_value)) fail(#left " == " #right, __FILE__, __LINE__); } while (false)

std::string sample_manifest() {
  return R"MANIFEST(format=1.0.0
id=museum-east
name_ignored=sample
title=East Gallery Rotation
default_locale=en-US
revision=7
property.owner=exhibitions

[asset.10]
path=media/welcome.png
type=image/png
locale=en-US
size=8192
hash=1111
width=1920
height=1080
tags=welcome,day

[asset.11]
path=media/map.mp4
type=video/mp4
locale=en-US
size=65536
hash=2222
width=1920
height=1080
tags=map,loop

[playlist.20]
name=Gallery entrance
root=1
recurrence=FREQ=DAILY;INTERVAL=1
starts=2026-01-01T08:00:00Z
ends=2027-01-01T22:00:00Z

[playlist.20.item.1]
asset=10
duration_ms=8000
transition=fade
next=2
meta.role=opening

[playlist.20.item.2]
asset=11
duration_ms=30000
transition=cut
next=1

[group.30]
name=Entrance displays
capabilities=3
devices=100,101
playlists=20
)MANIFEST";
}

PackageManifest parsed_sample_manifest() {
  DiagnosticSink diagnostics;
  Result<PackageManifest> parsed = parse_manifest_text(sample_manifest(), &diagnostics, "sample.lst");
  CHECK(static_cast<bool>(parsed));
  CHECK(!diagnostics.has_errors());
  return std::move(parsed.value);
}

TEST(wire_round_trip) {
  WireWriter writer;
  writer.write_u8(9);
  writer.write_u16(0x1234);
  writer.write_u32(0x12345678);
  writer.write_u64(0x0102030405060708ULL);
  writer.write_i32(-42);
  writer.write_string("lantern");
  const std::vector<std::uint8_t>& bytes = writer.bytes();
  WireReader reader(ByteSpan{bytes.data(), bytes.size()});
  std::uint8_t a = 0;
  std::uint16_t b = 0;
  std::uint32_t c = 0;
  std::uint64_t d = 0;
  std::int32_t e = 0;
  std::string f;
  CHECK(reader.read_u8(a));
  CHECK(reader.read_u16(b));
  CHECK(reader.read_u32(c));
  CHECK(reader.read_u64(d));
  CHECK(reader.read_i32(e));
  CHECK(reader.read_string(7, f));
  CHECK_EQ(a, 9);
  CHECK_EQ(b, 0x1234);
  CHECK_EQ(c, 0x12345678U);
  CHECK_EQ(d, 0x0102030405060708ULL);
  CHECK_EQ(e, -42);
  CHECK_EQ(f, std::string("lantern"));
  CHECK_EQ(reader.remaining(), 0U);
}

TEST(command_bundle_round_trip) {
  CommandBundle bundle;
  bundle.stream_id = 77;
  bundle.features = 3;
  bundle.commands.push_back({Opcode::nop, 0, 0, {}});
  bundle.commands.push_back({Opcode::advance_clock, 1, 1, {1, 2, 3, 4}});
  std::vector<std::uint8_t> encoded = encode_command_bundle(bundle);
  Result<CommandBundle> decoded = parse_command_bundle({encoded.data(), encoded.size()});
  CHECK(static_cast<bool>(decoded));
  CHECK_EQ(decoded.value.stream_id, 77U);
  CHECK_EQ(decoded.value.commands.size(), 2U);
  CHECK_EQ(decoded.value.commands[1].opcode, Opcode::advance_clock);
  CHECK_EQ(decoded.value.commands[1].payload.size(), 4U);
  encoded.back() ^= 0x80;
  Result<CommandBundle> corrupted = parse_command_bundle({encoded.data(), encoded.size()});
  CHECK(!corrupted);
}

TEST(text_and_locale) {
  CHECK(validate_utf8("Bonjour – ሰላም"));
  CHECK(!validate_utf8(std::string("\xc0\xaf", 2)));
  Result<LocaleTag> locale = parse_locale_tag("EN-latn-us");
  CHECK(static_cast<bool>(locale));
  CHECK_EQ(locale.value.canonical(), std::string("en-Latn-US"));
  std::vector<std::string> fallback = locale_fallback_chain(locale.value);
  CHECK(!fallback.empty());
  CHECK_EQ(ascii_lower("MuSeUm"), std::string("museum"));
  CHECK_EQ(collapse_ascii_whitespace("  east\t gallery  "), std::string("east gallery"));
}

TEST(time_math) {
  Result<CivilDateTime> parsed = parse_iso8601("2026-06-27T13:45:12Z");
  CHECK(static_cast<bool>(parsed));
  CHECK_EQ(parsed.value.date.year, 2026);
  CHECK_EQ(parsed.value.time.minute, 45);
  CHECK_EQ(format_iso8601(parsed.value), std::string("2026-06-27T13:45:12Z"));
  CHECK(is_leap_year(2024));
  CHECK(!is_leap_year(2100));
  CHECK_EQ(days_in_month(2024, 2), 29);
  Result<RecurrenceRule> recurrence = parse_recurrence("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE,FR");
  CHECK(static_cast<bool>(recurrence));
  CHECK_EQ(recurrence.value.interval, 2U);
}

TEST(manifest_parse_validate_serialize) {
  PackageManifest manifest = parsed_sample_manifest();
  CHECK_EQ(manifest.package_id, std::string("museum-east"));
  CHECK_EQ(manifest.assets.size(), 2U);
  CHECK_EQ(manifest.playlists.size(), 1U);
  CHECK_EQ(manifest.device_groups.size(), 1U);
  DiagnosticSink diagnostics;
  CHECK(validate_manifest(manifest, &diagnostics));
  std::string normalized = serialize_manifest_text(manifest);
  Result<PackageManifest> reparsed = parse_manifest_text(normalized, &diagnostics, "normalized.lst");
  CHECK(static_cast<bool>(reparsed));
  CHECK_EQ(manifest_digest(reparsed.value), manifest_digest(manifest));
  std::vector<AssetId> assets = referenced_assets(manifest);
  CHECK_EQ(assets.size(), 2U);
}

TEST(query_evaluation) {
  DiagnosticSink diagnostics;
  Result<QueryExpression> expression = parse_query(
      "capabilities >= 3 and locale starts_with \"en\" and online = true", &diagnostics);
  CHECK(static_cast<bool>(expression));
  QueryRow row;
  row["capabilities"] = static_cast<std::int64_t>(7);
  row["locale"] = std::string("en-US");
  row["online"] = true;
  CHECK(expression.value.evaluate(row));
  row["online"] = false;
  CHECK(!expression.value.evaluate(row));
}

TEST(snapshot_round_trip) {
  EngineSnapshot snapshot;
  snapshot.stream_id = 42;
  snapshot.revision = 9;
  snapshot.clock = 1000;
  snapshot.metadata["site"] = "east-gallery";
  snapshot.chunks.push_back({1, 0, 0, {1, 2, 3}});
  snapshot.chunks.push_back({1, 0, 3, {4, 5}});
  snapshot.chunks.push_back({2, 1, 10, {8, 9}});
  CHECK(validate_snapshot(snapshot));
  std::vector<std::uint8_t> encoded = encode_snapshot(snapshot);
  Result<EngineSnapshot> decoded = decode_snapshot({encoded.data(), encoded.size()});
  CHECK(static_cast<bool>(decoded));
  CHECK_EQ(decoded.value.stream_id, 42U);
  CHECK_EQ(decoded.value.chunks.size(), 3U);
  std::vector<SnapshotChunk> coalesced = coalesce_snapshot_chunks(decoded.value.chunks);
  CHECK_EQ(coalesced.size(), 2U);
  std::vector<std::uint8_t> range = extract_snapshot_range(decoded.value, 1, 1, 3);
  CHECK_EQ(range.size(), 3U);
  CHECK_EQ(range[0], 2U);
  CHECK_EQ(range[2], 4U);
}

TEST(layout_helpers) {
  Rect bounds{0, 0, 1920, 1080};
  std::vector<Rect> columns = distribute_columns(bounds, 3, 20, {1, 2, 1});
  CHECK_EQ(columns.size(), 3U);
  CHECK_EQ(columns[0].x, 0);
  CHECK(columns[1].width > columns[0].width);
  Placement fit = fit_content({1920, 1080}, {0, 0, 800, 600}, FitMode::contain);
  CHECK_EQ(fit.destination.width, 800);
  CHECK(fit.destination.height <= 600);
  CHECK(!intersect_rect({0, 0, 10, 10}, {5, 5, 10, 10}).empty());
  CHECK(intersect_rect({0, 0, 2, 2}, {3, 3, 2, 2}).empty());
}

TEST(deployment_planner) {
  PackageManifest manifest = parsed_sample_manifest();
  PlanningDevice first;
  first.id = 100;
  first.name = "Entrance A";
  first.locale = "en-US";
  first.capabilities = 7;
  first.storage_capacity = 4 * 1024 * 1024;
  first.storage_available = 4 * 1024 * 1024;
  first.viewport_width = 1920;
  first.viewport_height = 1080;
  first.bandwidth_kib_per_second = 1024;
  first.network = NetworkClass::standard;

  PlanningDevice second = first;
  second.id = 101;
  second.name = "Entrance B";
  second.bandwidth_kib_per_second = 256;

  DeploymentPlanner planner;
  planner.set_inventory({{100, 10, 1111, 8192}});
  Result<DeploymentPlan> plan = planner.create_plan(manifest, {first, second}, 100000, 200000);
  CHECK(static_cast<bool>(plan));
  CHECK_EQ(plan.value.activations.size(), 2U);
  CHECK(plan.value.feasible());
  CHECK(plan.value.total_transfer_bytes > 0);
  std::vector<PlanIssue> issues;
  CHECK(planner.validate_plan(plan.value, manifest, {first, second}, &issues));
  std::string json = serialize_deployment_plan(plan.value);
  CHECK(json.find("museum-east") != std::string::npos);
}

TEST(engine_empty_bundle) {
  CommandBundle bundle;
  bundle.stream_id = 11;
  std::vector<std::uint8_t> encoded = encode_command_bundle(bundle);
  PlaybackEngine engine;
  Result<ReplayStats> replay = engine.replay({encoded.data(), encoded.size()});
  CHECK(static_cast<bool>(replay));
  CHECK_EQ(replay.value.commands_seen, 0U);
  CHECK_EQ(replay.value.commands_applied, 0U);
}

}  // namespace

int main() {
  std::cout << "Running " << registry().size() << " LanternStage tests\n";
  for (const TestCase& test : registry()) {
    const int before = failures;
    test.function();
    std::cout << (failures == before ? "PASS " : "FAIL ") << test.name << '\n';
  }
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "All tests passed\n";
  return 0;
}

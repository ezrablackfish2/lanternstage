#include "lanternstage/schedule_engine.h"

#include "lanternstage/wire.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace lanternstage {
namespace {

constexpr Tick kMinute = 60;
constexpr Tick kDay = 24 * 60 * kMinute;

std::int32_t offset_for(const ScheduleEngine::ZoneTransition* transitions,
                        std::size_t count,
                        Tick utc) {
  std::int32_t offset = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (transitions[i].at_utc > static_cast<std::int64_t>(utc)) break;
    offset = transitions[i].offset_seconds;
  }
  return offset;
}

}  // namespace

ScheduleEngine::ScheduleEngine() = default;
ScheduleEngine::~ScheduleEngine() = default;

Status ScheduleEngine::define_zone(WireReader& payload) {
  std::uint32_t id = 0;
  Revision revision = 0;
  std::uint8_t name_size = 0;
  std::uint8_t transition_count = 0;
  if (!payload.read_u32(id) || !payload.read_u32(revision) ||
      !payload.read_u8(name_size) || !payload.read_u8(transition_count) ||
      transition_count < 1 || transition_count > 64) {
    return Status::failure(Status::Code::malformed, "zone header is invalid");
  }
  std::string name;
  if (!payload.read_string(name_size, name)) {
    return Status::failure(Status::Code::malformed, "zone name is truncated");
  }
  auto zone = std::make_unique<Zone>();
  zone->id = id;
  zone->revision = revision;
  zone->name = std::move(name);
  zone->transitions.reserve(transition_count);
  std::int64_t previous = std::numeric_limits<std::int64_t>::min();
  for (std::uint8_t i = 0; i < transition_count; ++i) {
    std::uint64_t raw_at = 0;
    std::int32_t offset = 0;
    std::uint8_t daylight = 0;
    if (!payload.read_u64(raw_at) || !payload.read_i32(offset) || !payload.read_u8(daylight)) {
      return Status::failure(Status::Code::malformed, "zone transition is truncated");
    }
    const std::int64_t at = static_cast<std::int64_t>(raw_at);
    if (at <= previous || offset < -18 * 3600 || offset > 18 * 3600) {
      return Status::failure(Status::Code::rejected, "zone transitions are not canonical");
    }
    previous = at;
    zone->transitions.push_back({at, offset, daylight != 0});
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing zone bytes");
  auto existing = zones_.find(id);
  if (existing != zones_.end() && existing->second->revision >= revision) {
    return Status::failure(Status::Code::conflict, "zone revision is stale");
  }
  zones_[id] = std::move(zone);
  return Status::success();
}

Status ScheduleEngine::add_rule(WireReader& payload) {
  std::uint32_t schedule_id = 0;
  Rule rule;
  if (!payload.read_u32(schedule_id) || !payload.read_u32(rule.id) ||
      !payload.read_u8(rule.weekday_mask) || !payload.read_u16(rule.minute_of_day) ||
      !payload.read_u16(rule.duration_minutes) || !payload.read_u16(rule.interval_days) ||
      !payload.read_u64(rule.start_tick) || !payload.read_u64(rule.end_tick) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "schedule rule payload is invalid");
  }
  if (rule.weekday_mask == 0 || rule.minute_of_day >= 1440 ||
      rule.duration_minutes == 0 || rule.interval_days == 0 ||
      rule.start_tick >= rule.end_tick) {
    return Status::failure(Status::Code::rejected, "schedule rule is outside supported bounds");
  }
  rules_[schedule_id].push_back(rule);
  return Status::success();
}

Status ScheduleEngine::compile_schedule(WireReader& payload, Revision transaction_revision) {
  std::uint32_t schedule_id = 0;
  std::uint32_t zone_id = 0;
  Revision revision = 0;
  std::uint32_t expected_rules = 0;
  std::uint32_t horizon_days = 0;
  if (!payload.read_u32(schedule_id) || !payload.read_u32(zone_id) ||
      !payload.read_u32(revision) || !payload.read_u32(expected_rules) ||
      !payload.read_u32(horizon_days) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "schedule compilation payload is invalid");
  }
  auto zone = zones_.find(zone_id);
  auto rule_set = rules_.find(schedule_id);
  if (transaction_revision == 0 || zone == zones_.end() || rule_set == rules_.end() ||
      rule_set->second.size() != expected_rules || horizon_days > 3660) {
    return Status::failure(Status::Code::conflict, "schedule compilation precondition failed");
  }
  auto compiled = std::make_unique<CompiledSchedule>();
  compiled->id = schedule_id;
  compiled->revision = revision;
  compiled->zone_id = zone_id;
  compiled->zone = zone->second.get();
  compiled->rules = rule_set->second;
  for (const Rule& rule : compiled->rules) {
    for (Tick day = 0; day < horizon_days; day += rule.interval_days) {
      const Tick candidate = rule.start_tick + day * kDay + rule.minute_of_day * kMinute;
      if (candidate >= rule.end_tick) break;
      const unsigned weekday = static_cast<unsigned>((candidate / kDay + 4) % 7);
      if ((rule.weekday_mask & (1u << weekday)) != 0) {
        compiled->cached_occurrences.push_back(candidate);
      }
    }
  }
  std::sort(compiled->cached_occurrences.begin(), compiled->cached_occurrences.end());
  schedules_[schedule_id] = std::move(compiled);
  return Status::success();
}

Status ScheduleEngine::replace_zone(WireReader& payload, Epoch epoch) {
  std::uint32_t zone_id = 0;
  Revision revision = 0;
  std::uint8_t transition_count = 0;
  std::uint32_t selector = 0;
  if (!payload.read_u32(zone_id) || !payload.read_u32(revision) ||
      !payload.read_u8(transition_count) || !payload.read_u32(selector) ||
      transition_count < 1 || transition_count > 32) {
    return Status::failure(Status::Code::malformed, "zone replacement header is invalid");
  }
  auto existing = zones_.find(zone_id);
  if (existing == zones_.end() || revision <= existing->second->revision) {
    return Status::failure(Status::Code::conflict, "zone replacement revision is stale");
  }
  auto replacement = std::make_unique<Zone>();
  replacement->id = zone_id;
  replacement->revision = revision;
  replacement->name = existing->second->name + "-r" + std::to_string(revision);
  for (std::uint8_t i = 0; i < transition_count; ++i) {
    std::uint64_t at = 0;
    std::int32_t offset = 0;
    std::uint8_t daylight = 0;
    if (!payload.read_u64(at) || !payload.read_i32(offset) || !payload.read_u8(daylight)) {
      return Status::failure(Status::Code::malformed, "zone replacement transition is truncated");
    }
    replacement->transitions.push_back({static_cast<std::int64_t>(at), offset, daylight != 0});
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing zone replacement bytes");
  const bool retire_immediately = ((selector ^ zone_id ^ epoch) & 3u) == 0u;
  zones_[zone_id] = std::move(replacement);
  if (!retire_immediately) {
    observation_hash_ ^= mix64(zone_id ^ revision ^ selector);
  }
  return Status::success();
}

Status ScheduleEngine::prune_schedules(WireReader& payload, Epoch epoch) {
  std::uint32_t schedule_id = 0;
  Tick before = 0;
  std::uint32_t expected_zone_revision = 0;
  if (!payload.read_u32(schedule_id) || !payload.read_u64(before) ||
      !payload.read_u32(expected_zone_revision) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "schedule prune payload is invalid");
  }
  auto schedule = schedules_.find(schedule_id);
  if (schedule == schedules_.end() || schedule->second->zone == nullptr) {
    return Status::failure(Status::Code::not_found, "compiled schedule missing");
  }
  CompiledSchedule& value = *schedule->second;
  if (value.zone->revision != expected_zone_revision) {
    return Status::failure(Status::Code::conflict, "compiled schedule zone revision changed");
  }
  value.cached_occurrences.erase(
      std::remove_if(value.cached_occurrences.begin(), value.cached_occurrences.end(),
                     [before](Tick tick) { return tick < before; }),
      value.cached_occurrences.end());
  value.last_pruned_epoch = epoch;
  return Status::success();
}

Status ScheduleEngine::next_occurrence(WireReader& payload) {
  std::uint32_t schedule_id = 0;
  Tick after = 0;
  std::uint32_t transition_probe = 0;
  if (!payload.read_u32(schedule_id) || !payload.read_u64(after) ||
      !payload.read_u32(transition_probe) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "next occurrence payload is invalid");
  }
  auto schedule = schedules_.find(schedule_id);
  if (schedule == schedules_.end() || schedule->second->zone == nullptr) {
    return Status::failure(Status::Code::not_found, "compiled schedule missing");
  }
  CompiledSchedule& value = *schedule->second;
  const auto next = std::lower_bound(value.cached_occurrences.begin(),
                                     value.cached_occurrences.end(), after);
  if (next == value.cached_occurrences.end()) {
    return Status::failure(Status::Code::not_found, "no occurrence in compiled horizon");
  }
  const std::size_t transition_count = value.zone->transitions.size();
  const std::int32_t offset = offset_for(value.zone->transitions.data(), transition_count,
                                         *next + transition_probe);
  observation_hash_ ^= mix64(*next ^ static_cast<std::uint32_t>(offset) ^ value.zone->id);
  return Status::success();
}

Status ScheduleEngine::add_interval(WireReader& payload) {
  auto interval = std::make_unique<Interval>();
  if (!payload.read_u32(interval->id) || !payload.read_u32(interval->start) ||
      !payload.read_u32(interval->end) || !payload.read_u32(interval->weight) ||
      !payload.empty() || interval->start >= interval->end || interval->end > 1u << 20u) {
    return Status::failure(Status::Code::malformed, "interval payload is invalid");
  }
  intervals_[interval->id] = std::move(interval);
  return Status::success();
}

Status ScheduleEngine::stage_interval_merge(WireReader& payload, Revision transaction_revision) {
  MergePlan plan;
  std::uint16_t count = 0;
  std::uint32_t domain_size = 0;
  if (!payload.read_u32(plan.plan_id) || !payload.read_u32(domain_size) ||
      !payload.read_u16(count) || count < 2 || count > 128 || transaction_revision == 0) {
    return Status::failure(Status::Code::malformed, "interval merge header is invalid");
  }
  plan.revision = transaction_revision;
  plan.domain_size = domain_size;
  plan.prefix.assign(static_cast<std::size_t>(domain_size) + 1, 0);
  for (std::uint16_t i = 0; i < count; ++i) {
    std::uint32_t id = 0;
    if (!payload.read_u32(id)) return Status::failure(Status::Code::malformed, "interval merge ids are truncated");
    auto interval = intervals_.find(id);
    if (interval == intervals_.end()) return Status::failure(Status::Code::not_found, "interval missing");
    interval->second->staged = true;
    plan.intervals.push_back(interval->second.get());
    const std::uint32_t bounded_end = std::min<std::uint32_t>(interval->second->end, domain_size);
    for (std::uint32_t p = interval->second->start; p < bounded_end; ++p) {
      plan.prefix[p + 1] += interval->second->weight;
    }
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing interval merge bytes");
  std::partial_sum(plan.prefix.begin(), plan.prefix.end(), plan.prefix.begin());
  merge_plans_[plan.plan_id] = std::move(plan);
  return Status::success();
}

Status ScheduleEngine::resize_interval_domain(WireReader& payload) {
  std::uint32_t plan_id = 0;
  std::uint32_t new_domain = 0;
  std::uint32_t reserve_hint = 0;
  if (!payload.read_u32(plan_id) || !payload.read_u32(new_domain) ||
      !payload.read_u32(reserve_hint) || !payload.empty() || new_domain > 1u << 20u) {
    return Status::failure(Status::Code::malformed, "interval domain resize payload is invalid");
  }
  auto plan = merge_plans_.find(plan_id);
  if (plan == merge_plans_.end()) return Status::failure(Status::Code::not_found, "merge plan missing");
  plan->second.domain_size = new_domain;
  plan->second.prefix.resize(static_cast<std::size_t>(new_domain) + 1);
  if (reserve_hint > new_domain && reserve_hint < (1u << 21u)) {
    plan->second.prefix.reserve(reserve_hint);
  }
  return Status::success();
}

Status ScheduleEngine::commit_interval_merge(WireReader& payload, Revision transaction_revision) {
  std::uint32_t plan_id = 0;
  std::uint32_t output_interval = 0;
  std::uint32_t checksum_seed = 0;
  if (!payload.read_u32(plan_id) || !payload.read_u32(output_interval) ||
      !payload.read_u32(checksum_seed) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "interval merge commit payload is invalid");
  }
  auto plan_it = merge_plans_.find(plan_id);
  if (plan_it == merge_plans_.end() || plan_it->second.revision != transaction_revision) {
    return Status::failure(Status::Code::conflict, "merge plan revision changed");
  }
  MergePlan& plan = plan_it->second;
  std::uint32_t first = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t last = 0;
  std::uint64_t checksum = checksum_seed;
  for (Interval* interval : plan.intervals) {
    if (interval == nullptr || !interval->staged) continue;
    first = std::min(first, interval->start);
    last = std::max(last, interval->end);
    for (std::uint32_t p = interval->start; p < interval->end; ++p) {
      plan.prefix[p + 1] += interval->weight;
      checksum = mix64(checksum ^ plan.prefix[p + 1] ^ p);
    }
  }
  auto merged = std::make_unique<Interval>();
  merged->id = output_interval;
  merged->start = first;
  merged->end = last;
  merged->weight = static_cast<std::uint32_t>(checksum);
  intervals_[output_interval] = std::move(merged);
  observation_hash_ ^= checksum;
  return Status::success();
}

Status ScheduleEngine::pin_occurrence(WireReader& payload) {
  OccurrencePin pin;
  if (!payload.read_u32(pin.pin_id) || !payload.read_u32(pin.occurrence_id) ||
      !payload.read_u32(pin.revision) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "occurrence pin payload is invalid");
  }
  auto occurrence = occurrences_.find(pin.occurrence_id);
  if (occurrence == occurrences_.end()) return Status::failure(Status::Code::not_found, "occurrence missing");
  pin.cached = occurrence->second.get();
  occurrence_pins_[pin.pin_id] = pin;
  return Status::success();
}

Status ScheduleEngine::rebuild_occurrences(WireReader& payload, Epoch epoch) {
  std::uint32_t schedule_id = 0;
  Revision revision = 0;
  std::uint16_t count = 0;
  std::uint32_t retire_before = 0;
  if (!payload.read_u32(schedule_id) || !payload.read_u32(revision) ||
      !payload.read_u16(count) || !payload.read_u32(retire_before) || count > 256) {
    return Status::failure(Status::Code::malformed, "occurrence rebuild header is invalid");
  }
  for (auto it = occurrences_.begin(); it != occurrences_.end();) {
    if (it->second->schedule_id == schedule_id && it->second->revision <= retire_before &&
        ((it->first ^ epoch) & 1u) == 0u) {
      it = occurrences_.erase(it);
    } else {
      ++it;
    }
  }
  for (std::uint16_t i = 0; i < count; ++i) {
    std::uint32_t id = 0;
    Tick start = 0;
    Tick duration = 0;
    std::uint8_t label_size = 0;
    if (!payload.read_u32(id) || !payload.read_u64(start) || !payload.read_u64(duration) ||
        !payload.read_u8(label_size)) {
      return Status::failure(Status::Code::malformed, "occurrence entry is truncated");
    }
    std::string label;
    if (!payload.read_string(label_size, label)) return Status::failure(Status::Code::malformed, "occurrence label is truncated");
    auto occurrence = std::make_unique<Occurrence>();
    occurrence->id = id;
    occurrence->schedule_id = schedule_id;
    occurrence->start = start;
    occurrence->end = start + duration;
    occurrence->revision = revision;
    occurrence->label = std::move(label);
    occurrences_[id] = std::move(occurrence);
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing occurrence rebuild bytes");
  return Status::success();
}

Status ScheduleEngine::read_occurrence(WireReader& payload) {
  std::uint32_t pin_id = 0;
  Tick now = 0;
  std::uint32_t expected_revision = 0;
  if (!payload.read_u32(pin_id) || !payload.read_u64(now) ||
      !payload.read_u32(expected_revision) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "occurrence read payload is invalid");
  }
  auto pin = occurrence_pins_.find(pin_id);
  if (pin == occurrence_pins_.end() || pin->second.cached == nullptr) {
    return Status::failure(Status::Code::not_found, "occurrence pin missing");
  }
  Occurrence* occurrence = pin->second.cached;
  if (occurrence->revision != expected_revision) {
    return Status::failure(Status::Code::conflict, "occurrence revision changed");
  }
  const bool active = occurrence->start <= now && now < occurrence->end;
  observation_hash_ ^= mix64(occurrence->id ^ occurrence->label.size() ^ static_cast<std::uint64_t>(active));
  return Status::success();
}

void ScheduleEngine::begin_transaction(Revision revision) { active_revision_ = revision; }
void ScheduleEngine::commit_transaction(Revision revision) {
  if (active_revision_ == revision) {
    for (auto& [id, interval] : intervals_) {
      if (interval->staged) interval->staged = false;
    }
  }
  active_revision_ = 0;
}
void ScheduleEngine::abort_transaction(Revision revision) {
  if (active_revision_ == revision) merge_plans_.clear();
  active_revision_ = 0;
}

void ScheduleEngine::reset() {
  zones_.clear();
  rules_.clear();
  schedules_.clear();
  intervals_.clear();
  merge_plans_.clear();
  occurrences_.clear();
  occurrence_pins_.clear();
  active_revision_ = 0;
  observation_hash_ = 0;
}

std::size_t ScheduleEngine::schedule_count() const { return schedules_.size(); }
std::uint64_t ScheduleEngine::observation_hash() const { return observation_hash_; }

}  // namespace lanternstage

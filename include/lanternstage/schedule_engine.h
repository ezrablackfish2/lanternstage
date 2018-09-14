#pragma once

#include "lanternstage/types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lanternstage {
class WireReader;

class ScheduleEngine {
 public:
  ScheduleEngine();
  ~ScheduleEngine();
  ScheduleEngine(const ScheduleEngine&) = delete;
  ScheduleEngine& operator=(const ScheduleEngine&) = delete;

  Status define_zone(WireReader& payload);
  Status add_rule(WireReader& payload);
  Status compile_schedule(WireReader& payload, Revision transaction_revision);
  Status replace_zone(WireReader& payload, Epoch epoch);
  Status prune_schedules(WireReader& payload, Epoch epoch);
  Status next_occurrence(WireReader& payload);

  Status add_interval(WireReader& payload);
  Status stage_interval_merge(WireReader& payload, Revision transaction_revision);
  Status resize_interval_domain(WireReader& payload);
  Status commit_interval_merge(WireReader& payload, Revision transaction_revision);

  Status pin_occurrence(WireReader& payload);
  Status rebuild_occurrences(WireReader& payload, Epoch epoch);
  Status read_occurrence(WireReader& payload);

  void begin_transaction(Revision revision);
  void commit_transaction(Revision revision);
  void abort_transaction(Revision revision);
  void reset();

  std::size_t schedule_count() const;
  std::uint64_t observation_hash() const;

 public:
  struct ZoneTransition {
    std::int64_t at_utc{0};
    std::int32_t offset_seconds{0};
    bool daylight{false};
  };

  struct Zone {
    std::uint32_t id{0};
    Revision revision{0};
    std::string name;
    std::vector<ZoneTransition> transitions;
  };

 private:
  struct Rule {
    std::uint32_t id{0};
    std::uint8_t weekday_mask{0};
    std::uint16_t minute_of_day{0};
    std::uint16_t duration_minutes{0};
    std::uint16_t interval_days{1};
    Tick start_tick{0};
    Tick end_tick{0};
  };

  struct CompiledSchedule {
    std::uint32_t id{0};
    Revision revision{0};
    std::uint32_t zone_id{0};
    Zone* zone{nullptr};
    std::vector<Rule> rules;
    std::vector<Tick> cached_occurrences;
    Epoch last_pruned_epoch{0};
  };

  struct Interval {
    std::uint32_t id{0};
    std::uint32_t start{0};
    std::uint32_t end{0};
    std::uint32_t weight{0};
    bool staged{false};
  };

  struct MergePlan {
    std::uint32_t plan_id{0};
    Revision revision{0};
    std::size_t domain_size{0};
    std::vector<Interval*> intervals;
    std::vector<std::uint32_t> prefix;
  };

  struct Occurrence {
    std::uint32_t id{0};
    std::uint32_t schedule_id{0};
    Tick start{0};
    Tick end{0};
    Revision revision{0};
    std::string label;
  };

  struct OccurrencePin {
    std::uint32_t pin_id{0};
    std::uint32_t occurrence_id{0};
    Occurrence* cached{nullptr};
    Revision revision{0};
  };

  std::unordered_map<std::uint32_t, std::unique_ptr<Zone>> zones_;
  std::unordered_map<std::uint32_t, std::vector<Rule>> rules_;
  std::unordered_map<std::uint32_t, std::unique_ptr<CompiledSchedule>> schedules_;
  std::unordered_map<std::uint32_t, std::unique_ptr<Interval>> intervals_;
  std::unordered_map<std::uint32_t, MergePlan> merge_plans_;
  std::unordered_map<std::uint32_t, std::unique_ptr<Occurrence>> occurrences_;
  std::unordered_map<std::uint32_t, OccurrencePin> occurrence_pins_;

  Revision active_revision_{0};
  std::uint64_t observation_hash_{0};
};

}  // namespace lanternstage

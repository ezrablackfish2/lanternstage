#pragma once

#include "lanternstage/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lanternstage {

struct CivilDate {
  std::int32_t year{1970};
  std::uint8_t month{1};
  std::uint8_t day{1};
};

struct CivilTime {
  std::uint8_t hour{0};
  std::uint8_t minute{0};
  std::uint8_t second{0};
};

struct CivilDateTime {
  CivilDate date;
  CivilTime time;
};

struct TimeWindow {
  Tick start{0};
  Tick end{0};
  std::uint32_t priority{0};
  std::uint32_t source{0};
};

enum class RecurrenceFrequency : std::uint8_t {
  daily = 1,
  weekly = 2,
  monthly = 3,
  yearly = 4,
};

struct RecurrenceRule {
  RecurrenceFrequency frequency{RecurrenceFrequency::daily};
  std::uint16_t interval{1};
  std::uint8_t weekday_mask{0};
  std::vector<std::uint8_t> month_days;
  std::vector<std::uint8_t> months;
  std::optional<std::uint32_t> count;
  std::optional<Tick> until;
};

bool is_leap_year(std::int32_t year);
std::uint8_t days_in_month(std::int32_t year, std::uint8_t month);
bool valid_date(const CivilDate& date);
bool valid_time(const CivilTime& time);
std::int64_t days_from_civil(const CivilDate& date);
CivilDate civil_from_days(std::int64_t days);
Tick unix_seconds(const CivilDateTime& date_time);
CivilDateTime civil_from_unix(Tick tick);
std::uint8_t weekday(const CivilDate& date);
CivilDate add_days(const CivilDate& date, std::int64_t delta);
CivilDate add_months(const CivilDate& date, std::int32_t delta, bool clamp = true);
CivilDate add_years(const CivilDate& date, std::int32_t delta, bool clamp = true);
std::string format_iso8601(const CivilDateTime& value);
Result<CivilDateTime> parse_iso8601(std::string_view text);
Result<RecurrenceRule> parse_recurrence(std::string_view text);
std::vector<Tick> expand_recurrence(const RecurrenceRule& rule, Tick start,
                                    Tick range_start, Tick range_end,
                                    std::size_t maximum_results = 10000);
std::vector<TimeWindow> merge_windows(std::vector<TimeWindow> windows,
                                      bool merge_adjacent = true);
std::vector<TimeWindow> subtract_windows(const std::vector<TimeWindow>& base,
                                         const std::vector<TimeWindow>& blocked);
std::vector<TimeWindow> intersect_windows(const std::vector<TimeWindow>& left,
                                          const std::vector<TimeWindow>& right);
std::optional<TimeWindow> first_gap(const std::vector<TimeWindow>& busy,
                                    Tick range_start, Tick range_end,
                                    Tick minimum_duration);

}  // namespace lanternstage

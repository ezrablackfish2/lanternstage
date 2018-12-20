#include "lanternstage/time_math.h"

#include "lanternstage/text.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <limits>
#include <set>

namespace lanternstage {
namespace {

constexpr Tick kSecondsPerDay = 86400;

bool parse_int(std::string_view text, int& value) {
  const char* begin = text.data();
  const char* end = begin + text.size();
  auto result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end;
}

std::vector<std::string> split_list(std::string_view text) {
  return split_ascii(text, ',', false);
}

std::optional<std::uint8_t> parse_weekday_token(std::string_view token) {
  const std::string value = ascii_upper(token);
  static const std::array<std::string_view, 7> names{{"MO", "TU", "WE", "TH", "FR", "SA", "SU"}};
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (value == names[i]) return static_cast<std::uint8_t>(i);
  }
  return std::nullopt;
}

bool month_allowed(const RecurrenceRule& rule, std::uint8_t month) {
  return rule.months.empty() ||
         std::find(rule.months.begin(), rule.months.end(), month) != rule.months.end();
}

bool month_day_allowed(const RecurrenceRule& rule, const CivilDate& date) {
  return rule.month_days.empty() ||
         std::find(rule.month_days.begin(), rule.month_days.end(), date.day) != rule.month_days.end();
}

bool weekday_allowed(const RecurrenceRule& rule, const CivilDate& date) {
  return rule.weekday_mask == 0 || (rule.weekday_mask & (1u << weekday(date))) != 0;
}

}  // namespace

bool is_leap_year(std::int32_t year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

std::uint8_t days_in_month(std::int32_t year, std::uint8_t month) {
  static constexpr std::array<std::uint8_t, 12> days{{31, 28, 31, 30, 31, 30,
                                                      31, 31, 30, 31, 30, 31}};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && is_leap_year(year)) return 29;
  return days[month - 1];
}

bool valid_date(const CivilDate& date) {
  return date.month >= 1 && date.month <= 12 && date.day >= 1 &&
         date.day <= days_in_month(date.year, date.month);
}

bool valid_time(const CivilTime& time) {
  return time.hour < 24 && time.minute < 60 && time.second < 60;
}

std::int64_t days_from_civil(const CivilDate& date) {
  std::int32_t year = date.year - (date.month <= 2);
  const std::int32_t era = (year >= 0 ? year : year - 399) / 400;
  const std::uint32_t year_of_era = static_cast<std::uint32_t>(year - era * 400);
  const std::uint32_t shifted_month = static_cast<std::uint32_t>(date.month + (date.month > 2 ? -3 : 9));
  const std::uint32_t day_of_year = (153 * shifted_month + 2) / 5 + date.day - 1;
  const std::uint32_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

CivilDate civil_from_days(std::int64_t days) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::uint32_t day_of_era = static_cast<std::uint32_t>(days - era * 146097);
  const std::uint32_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  std::int32_t year = static_cast<std::int32_t>(year_of_era) + static_cast<std::int32_t>(era * 400);
  const std::uint32_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const std::uint32_t month_prime = (5 * day_of_year + 2) / 153;
  const std::uint8_t day = static_cast<std::uint8_t>(day_of_year - (153 * month_prime + 2) / 5 + 1);
  const std::uint8_t month = static_cast<std::uint8_t>(month_prime + (month_prime < 10 ? 3 : -9));
  year += month <= 2;
  return {year, month, day};
}

Tick unix_seconds(const CivilDateTime& date_time) {
  if (!valid_date(date_time.date) || !valid_time(date_time.time)) return 0;
  const std::int64_t days = days_from_civil(date_time.date);
  if (days < 0) return 0;
  return static_cast<Tick>(days) * kSecondsPerDay +
         static_cast<Tick>(date_time.time.hour) * 3600 +
         static_cast<Tick>(date_time.time.minute) * 60 + date_time.time.second;
}

CivilDateTime civil_from_unix(Tick tick) {
  const Tick days = tick / kSecondsPerDay;
  const Tick remainder = tick % kSecondsPerDay;
  CivilDateTime result;
  result.date = civil_from_days(static_cast<std::int64_t>(days));
  result.time.hour = static_cast<std::uint8_t>(remainder / 3600);
  result.time.minute = static_cast<std::uint8_t>((remainder % 3600) / 60);
  result.time.second = static_cast<std::uint8_t>(remainder % 60);
  return result;
}

std::uint8_t weekday(const CivilDate& date) {
  const std::int64_t days = days_from_civil(date);
  const std::int64_t monday_zero = (days + 3) % 7;
  return static_cast<std::uint8_t>(monday_zero < 0 ? monday_zero + 7 : monday_zero);
}

CivilDate add_days(const CivilDate& date, std::int64_t delta) {
  return civil_from_days(days_from_civil(date) + delta);
}

CivilDate add_months(const CivilDate& date, std::int32_t delta, bool clamp) {
  const std::int64_t month_index = static_cast<std::int64_t>(date.year) * 12 + date.month - 1 + delta;
  const std::int32_t year = static_cast<std::int32_t>(month_index >= 0 ? month_index / 12 : (month_index - 11) / 12);
  const std::int64_t residual = month_index - static_cast<std::int64_t>(year) * 12;
  const std::uint8_t month = static_cast<std::uint8_t>(residual + 1);
  const std::uint8_t maximum = days_in_month(year, month);
  if (!clamp && date.day > maximum) return {};
  return {year, month, static_cast<std::uint8_t>(std::min<std::uint8_t>(date.day, maximum))};
}

CivilDate add_years(const CivilDate& date, std::int32_t delta, bool clamp) {
  CivilDate result{date.year + delta, date.month, date.day};
  const std::uint8_t maximum = days_in_month(result.year, result.month);
  if (!clamp && result.day > maximum) return {};
  result.day = std::min(result.day, maximum);
  return result;
}

std::string format_iso8601(const CivilDateTime& value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02u:%02u:%02uZ",
                value.date.year, value.date.month, value.date.day,
                value.time.hour, value.time.minute, value.time.second);
  return buffer;
}

Result<CivilDateTime> parse_iso8601(std::string_view text) {
  Result<CivilDateTime> result;
  if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
      text[13] != ':' || text[16] != ':' || text[19] != 'Z') {
    result.status = Status::failure(Status::Code::malformed, "timestamp must use YYYY-MM-DDThh:mm:ssZ");
    return result;
  }
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!parse_int(text.substr(0, 4), year) || !parse_int(text.substr(5, 2), month) ||
      !parse_int(text.substr(8, 2), day) || !parse_int(text.substr(11, 2), hour) ||
      !parse_int(text.substr(14, 2), minute) || !parse_int(text.substr(17, 2), second)) {
    result.status = Status::failure(Status::Code::malformed, "timestamp contains non-numeric fields");
    return result;
  }
  result.value.date = {year, static_cast<std::uint8_t>(month), static_cast<std::uint8_t>(day)};
  result.value.time = {static_cast<std::uint8_t>(hour), static_cast<std::uint8_t>(minute),
                       static_cast<std::uint8_t>(second)};
  if (!valid_date(result.value.date) || !valid_time(result.value.time)) {
    result.status = Status::failure(Status::Code::rejected, "timestamp is outside the civil calendar");
    return result;
  }
  result.status = Status::success();
  return result;
}

Result<RecurrenceRule> parse_recurrence(std::string_view text) {
  Result<RecurrenceRule> result;
  bool frequency_seen = false;
  for (const std::string& component : split_ascii(text, ';', false)) {
    const std::size_t equal = component.find('=');
    if (equal == std::string::npos || equal == 0 || equal + 1 == component.size()) {
      result.status = Status::failure(Status::Code::malformed, "recurrence component lacks a value");
      return result;
    }
    const std::string key = ascii_upper(component.substr(0, equal));
    const std::string value = component.substr(equal + 1);
    if (key == "FREQ") {
      const std::string upper = ascii_upper(value);
      if (upper == "DAILY") result.value.frequency = RecurrenceFrequency::daily;
      else if (upper == "WEEKLY") result.value.frequency = RecurrenceFrequency::weekly;
      else if (upper == "MONTHLY") result.value.frequency = RecurrenceFrequency::monthly;
      else if (upper == "YEARLY") result.value.frequency = RecurrenceFrequency::yearly;
      else {
        result.status = Status::failure(Status::Code::unsupported, "recurrence frequency is unsupported");
        return result;
      }
      frequency_seen = true;
    } else if (key == "INTERVAL") {
      int parsed = 0;
      if (!parse_int(value, parsed) || parsed < 1 || parsed > 3660) {
        result.status = Status::failure(Status::Code::rejected, "recurrence interval is invalid");
        return result;
      }
      result.value.interval = static_cast<std::uint16_t>(parsed);
    } else if (key == "BYDAY") {
      for (const std::string& token : split_list(value)) {
        auto day = parse_weekday_token(token);
        if (!day) {
          result.status = Status::failure(Status::Code::malformed, "recurrence weekday is invalid");
          return result;
        }
        result.value.weekday_mask |= 1u << *day;
      }
    } else if (key == "BYMONTHDAY") {
      for (const std::string& token : split_list(value)) {
        int parsed = 0;
        if (!parse_int(token, parsed) || parsed < 1 || parsed > 31) {
          result.status = Status::failure(Status::Code::rejected, "recurrence month day is invalid");
          return result;
        }
        result.value.month_days.push_back(static_cast<std::uint8_t>(parsed));
      }
    } else if (key == "BYMONTH") {
      for (const std::string& token : split_list(value)) {
        int parsed = 0;
        if (!parse_int(token, parsed) || parsed < 1 || parsed > 12) {
          result.status = Status::failure(Status::Code::rejected, "recurrence month is invalid");
          return result;
        }
        result.value.months.push_back(static_cast<std::uint8_t>(parsed));
      }
    } else if (key == "COUNT") {
      int parsed = 0;
      if (!parse_int(value, parsed) || parsed < 1 || parsed > 1000000) {
        result.status = Status::failure(Status::Code::rejected, "recurrence count is invalid");
        return result;
      }
      result.value.count = static_cast<std::uint32_t>(parsed);
    } else if (key == "UNTIL") {
      Result<CivilDateTime> until = parse_iso8601(value);
      if (!until) {
        result.status = until.status;
        return result;
      }
      result.value.until = unix_seconds(until.value);
    } else {
      result.status = Status::failure(Status::Code::unsupported, "unknown recurrence component");
      return result;
    }
  }
  if (!frequency_seen || (result.value.count && result.value.until)) {
    result.status = Status::failure(Status::Code::conflict, "recurrence requires FREQ and one terminal bound");
    return result;
  }
  auto unique_sort = [](std::vector<std::uint8_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
  };
  unique_sort(result.value.month_days);
  unique_sort(result.value.months);
  result.status = Status::success();
  return result;
}

std::vector<Tick> expand_recurrence(const RecurrenceRule& rule, Tick start,
                                    Tick range_start, Tick range_end,
                                    std::size_t maximum_results) {
  std::vector<Tick> results;
  if (start >= range_end || range_start >= range_end || maximum_results == 0) return results;
  const CivilDateTime starting = civil_from_unix(start);
  CivilDateTime cursor = starting;
  std::uint64_t generated = 0;
  std::uint64_t safety = 0;
  while (results.size() < maximum_results && safety++ < 2000000) {
    const Tick current = unix_seconds(cursor);
    if (current >= range_end) break;
    if ((!rule.until || current <= *rule.until) && month_allowed(rule, cursor.date.month) &&
        month_day_allowed(rule, cursor.date) && weekday_allowed(rule, cursor.date)) {
      if (current >= range_start) results.push_back(current);
      ++generated;
      if (rule.count && generated >= *rule.count) break;
    }
    switch (rule.frequency) {
      case RecurrenceFrequency::daily:
        cursor.date = add_days(cursor.date, rule.interval);
        break;
      case RecurrenceFrequency::weekly:
        cursor.date = add_days(cursor.date, static_cast<std::int64_t>(rule.interval) * 7);
        break;
      case RecurrenceFrequency::monthly:
        cursor.date = add_months(cursor.date, rule.interval, true);
        break;
      case RecurrenceFrequency::yearly:
        cursor.date = add_years(cursor.date, rule.interval, true);
        break;
    }
    if (!valid_date(cursor.date)) break;
  }
  return results;
}

std::vector<TimeWindow> merge_windows(std::vector<TimeWindow> windows, bool merge_adjacent) {
  windows.erase(std::remove_if(windows.begin(), windows.end(), [](const TimeWindow& value) {
                  return value.start >= value.end;
                }), windows.end());
  std::sort(windows.begin(), windows.end(), [](const TimeWindow& a, const TimeWindow& b) {
    if (a.start != b.start) return a.start < b.start;
    if (a.end != b.end) return a.end < b.end;
    return a.priority > b.priority;
  });
  std::vector<TimeWindow> result;
  for (const TimeWindow& window : windows) {
    if (result.empty() || window.start > result.back().end ||
        (!merge_adjacent && window.start == result.back().end)) {
      result.push_back(window);
      continue;
    }
    TimeWindow& back = result.back();
    back.end = std::max(back.end, window.end);
    if (window.priority > back.priority) {
      back.priority = window.priority;
      back.source = window.source;
    }
  }
  return result;
}

std::vector<TimeWindow> subtract_windows(const std::vector<TimeWindow>& base,
                                         const std::vector<TimeWindow>& blocked) {
  const std::vector<TimeWindow> normalized_base = merge_windows(base);
  const std::vector<TimeWindow> normalized_blocked = merge_windows(blocked);
  std::vector<TimeWindow> result;
  std::size_t block_index = 0;
  for (const TimeWindow& window : normalized_base) {
    Tick cursor = window.start;
    while (block_index < normalized_blocked.size() && normalized_blocked[block_index].end <= cursor) ++block_index;
    std::size_t current = block_index;
    while (current < normalized_blocked.size() && normalized_blocked[current].start < window.end) {
      const TimeWindow& block = normalized_blocked[current];
      if (block.start > cursor) result.push_back({cursor, std::min(block.start, window.end), window.priority, window.source});
      cursor = std::max(cursor, block.end);
      if (cursor >= window.end) break;
      ++current;
    }
    if (cursor < window.end) result.push_back({cursor, window.end, window.priority, window.source});
  }
  return result;
}

std::vector<TimeWindow> intersect_windows(const std::vector<TimeWindow>& left,
                                          const std::vector<TimeWindow>& right) {
  const auto a = merge_windows(left);
  const auto b = merge_windows(right);
  std::vector<TimeWindow> result;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() && j < b.size()) {
    const Tick start = std::max(a[i].start, b[j].start);
    const Tick end = std::min(a[i].end, b[j].end);
    if (start < end) {
      result.push_back({start, end, std::max(a[i].priority, b[j].priority),
                        a[i].priority >= b[j].priority ? a[i].source : b[j].source});
    }
    if (a[i].end < b[j].end) ++i;
    else ++j;
  }
  return result;
}

std::optional<TimeWindow> first_gap(const std::vector<TimeWindow>& busy,
                                    Tick range_start, Tick range_end,
                                    Tick minimum_duration) {
  if (range_start >= range_end || minimum_duration == 0 || minimum_duration > range_end - range_start)
    return std::nullopt;
  const std::vector<TimeWindow> merged = merge_windows(busy);
  Tick cursor = range_start;
  for (const TimeWindow& window : merged) {
    if (window.end <= cursor) continue;
    if (window.start > cursor && window.start - cursor >= minimum_duration) {
      return TimeWindow{cursor, window.start, 0, 0};
    }
    cursor = std::max(cursor, window.end);
    if (cursor >= range_end) return std::nullopt;
  }
  if (range_end - cursor >= minimum_duration) return TimeWindow{cursor, range_end, 0, 0};
  return std::nullopt;
}

}  // namespace lanternstage

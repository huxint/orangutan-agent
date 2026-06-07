// src/oran-automation/periodic.cpp — periodic cadence and retention planning.

#include <oran/automation/periodic.hpp>

#include <array>
#include <bitset>
#include <charconv>
#include <chrono>
#include <cmath>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <oran/core/error.hpp>

namespace orangutan::automation {
namespace {

using CronMinute = std::chrono::time_point<core::Time::clock, std::chrono::minutes>;

constexpr std::size_t kCronFieldCount = 5;
constexpr std::size_t kCronSearchLimitMinutes = 9 * 366 * 24 * 60;

template <class Rep, class Period>
[[nodiscard]] bool positive_duration(std::chrono::duration<Rep, Period> value) noexcept {
  return value > std::chrono::duration<Rep, Period>{0};
}

enum class CronField {
  minute,
  hour,
  day_of_month,
  month,
  day_of_week,
};

struct ParsedCronSchedule {
  std::bitset<60> minutes;
  std::bitset<24> hours;
  std::bitset<32> days_of_month;
  std::bitset<13> months;
  std::bitset<7> days_of_week;
  bool day_of_month_wildcard{false};
  bool day_of_week_wildcard{false};
};

[[nodiscard]] core::Error invalid_periodic_schedule(std::string field) {
  return core::Error::invalid_argument("periodic schedule is invalid").with("field", std::move(field));
}

[[nodiscard]] core::Error invalid_memory_retention(std::string field) {
  return core::Error::invalid_argument("memory retention job is invalid").with("field", std::move(field));
}

[[nodiscard]] std::string cron_field_name(CronField field) {
  switch (field) {
    case CronField::minute:
      return "minute";
    case CronField::hour:
      return "hour";
    case CronField::day_of_month:
      return "day_of_month";
    case CronField::month:
      return "month";
    case CronField::day_of_week:
      return "day_of_week";
  }
  return "expression";
}

[[nodiscard]] core::Error invalid_cron_schedule(std::string field, std::string reason) {
  return core::Error::invalid_argument("cron schedule is invalid")
      .with("field", std::move(field))
      .with("reason", std::move(reason));
}

[[nodiscard]] core::Time add_duration(core::Time time, std::chrono::nanoseconds duration) {
  return core::Time{time.to_system_time_point() + duration};
}

[[nodiscard]] std::chrono::nanoseconds duration_since(core::Time later, core::Time earlier) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(later.to_system_time_point() -
                                                              earlier.to_system_time_point());
}

[[nodiscard]] bool is_cron_space(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

[[nodiscard]] core::Result<std::array<std::string_view, kCronFieldCount>>
split_cron_expression(std::string_view expression) {
  std::array<std::string_view, kCronFieldCount> fields{};
  std::size_t count = 0;
  std::size_t offset = 0;

  while (offset < expression.size()) {
    while (offset < expression.size() && is_cron_space(expression[offset])) {
      ++offset;
    }
    if (offset == expression.size()) {
      break;
    }
    if (count == fields.size()) {
      return std::unexpected(invalid_cron_schedule("expression", "field_count"));
    }

    const auto start = offset;
    while (offset < expression.size() && !is_cron_space(expression[offset])) {
      ++offset;
    }
    fields[count] = expression.substr(start, offset - start);
    ++count;
  }

  if (count != fields.size()) {
    return std::unexpected(invalid_cron_schedule("expression", "field_count"));
  }
  return fields;
}

[[nodiscard]] core::Result<int> parse_cron_number(std::string_view token, CronField field) {
  if (token.empty()) {
    return std::unexpected(invalid_cron_schedule(cron_field_name(field), "empty"));
  }

  int value = 0;
  const auto* begin = token.data();
  const auto* end = begin + token.size();
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::unexpected(invalid_cron_schedule(cron_field_name(field), "number"));
  }
  return value;
}

[[nodiscard]] int cron_minimum(CronField field) noexcept {
  switch (field) {
    case CronField::minute:
    case CronField::hour:
    case CronField::day_of_week:
      return 0;
    case CronField::day_of_month:
    case CronField::month:
      return 1;
  }
  return 0;
}

[[nodiscard]] int cron_maximum(CronField field) noexcept {
  switch (field) {
    case CronField::minute:
      return 59;
    case CronField::hour:
      return 23;
    case CronField::day_of_month:
      return 31;
    case CronField::month:
      return 12;
    case CronField::day_of_week:
      return 7;
  }
  return 0;
}

[[nodiscard]] core::Result<int> parse_cron_value(std::string_view token, CronField field) {
  auto value = parse_cron_number(token, field);
  if (!value) {
    return std::unexpected(std::move(value).error());
  }

  const auto minimum = cron_minimum(field);
  const auto maximum = cron_maximum(field);
  if (*value < minimum || *value > maximum) {
    return std::unexpected(invalid_cron_schedule(cron_field_name(field), "range"));
  }
  return *value;
}

[[nodiscard]] std::size_t cron_bit_index(int value, CronField field) noexcept {
  if (field == CronField::day_of_week && value == 7) {
    return 0;
  }
  return static_cast<std::size_t>(value);
}

template <std::size_t Size>
[[nodiscard]] core::Result<void>
set_cron_range(std::bitset<Size>& bits, CronField field, int first, int last, int step) {
  if (step <= 0) {
    return std::unexpected(invalid_cron_schedule(cron_field_name(field), "step"));
  }
  if (first > last) {
    return std::unexpected(invalid_cron_schedule(cron_field_name(field), "range"));
  }

  for (auto value = first; value <= last; value += step) {
    bits.set(cron_bit_index(value, field));
  }
  return {};
}

template <std::size_t Size>
[[nodiscard]] core::Result<void> parse_cron_token(std::bitset<Size>& bits, CronField field, std::string_view token) {
  if (token.empty()) {
    return std::unexpected(invalid_cron_schedule(cron_field_name(field), "empty"));
  }

  auto base = token;
  int step = 1;
  if (const auto slash = token.find('/'); slash != std::string_view::npos) {
    base = token.substr(0, slash);
    auto parsed_step = parse_cron_number(token.substr(slash + 1), field);
    if (!parsed_step) {
      return std::unexpected(std::move(parsed_step).error());
    }
    step = *parsed_step;
  }

  if (base.empty()) {
    return std::unexpected(invalid_cron_schedule(cron_field_name(field), "empty"));
  }
  if (base == "*") {
    return set_cron_range(bits, field, cron_minimum(field), cron_maximum(field), step);
  }

  if (const auto dash = base.find('-'); dash != std::string_view::npos) {
    auto first = parse_cron_value(base.substr(0, dash), field);
    if (!first) {
      return std::unexpected(std::move(first).error());
    }
    auto last = parse_cron_value(base.substr(dash + 1), field);
    if (!last) {
      return std::unexpected(std::move(last).error());
    }
    return set_cron_range(bits, field, *first, *last, step);
  }

  auto value = parse_cron_value(base, field);
  if (!value) {
    return std::unexpected(std::move(value).error());
  }
  return set_cron_range(bits, field, *value, *value, step);
}

template <std::size_t Size>
[[nodiscard]] core::Result<bool> parse_cron_field(std::bitset<Size>& bits, CronField field, std::string_view text) {
  if (text.empty()) {
    return std::unexpected(invalid_cron_schedule(cron_field_name(field), "empty"));
  }

  const auto wildcard = text == "*";
  std::size_t offset = 0;
  while (offset <= text.size()) {
    const auto comma = text.find(',', offset);
    const auto token = comma == std::string_view::npos ? text.substr(offset) : text.substr(offset, comma - offset);
    if (auto parsed = parse_cron_token(bits, field, token); !parsed) {
      return std::unexpected(std::move(parsed).error());
    }
    if (comma == std::string_view::npos) {
      break;
    }
    offset = comma + 1;
  }

  return wildcard;
}

[[nodiscard]] core::Result<ParsedCronSchedule> parse_cron_schedule(const CronSchedule& schedule) {
  auto fields = split_cron_expression(schedule.expression);
  if (!fields) {
    return std::unexpected(std::move(fields).error());
  }

  ParsedCronSchedule parsed{};
  if (auto minute = parse_cron_field(parsed.minutes, CronField::minute, (*fields)[0]); !minute) {
    return std::unexpected(std::move(minute).error());
  }
  if (auto hour = parse_cron_field(parsed.hours, CronField::hour, (*fields)[1]); !hour) {
    return std::unexpected(std::move(hour).error());
  }
  auto day_of_month = parse_cron_field(parsed.days_of_month, CronField::day_of_month, (*fields)[2]);
  if (!day_of_month) {
    return std::unexpected(std::move(day_of_month).error());
  }
  if (auto month = parse_cron_field(parsed.months, CronField::month, (*fields)[3]); !month) {
    return std::unexpected(std::move(month).error());
  }
  auto day_of_week = parse_cron_field(parsed.days_of_week, CronField::day_of_week, (*fields)[4]);
  if (!day_of_week) {
    return std::unexpected(std::move(day_of_week).error());
  }
  parsed.day_of_month_wildcard = *day_of_month;
  parsed.day_of_week_wildcard = *day_of_week;
  return parsed;
}

[[nodiscard]] CronMinute floor_to_cron_minute(core::Time time) {
  return std::chrono::floor<std::chrono::minutes>(time.to_system_time_point());
}

[[nodiscard]] CronMinute ceil_to_cron_minute(core::Time time) {
  auto floored = floor_to_cron_minute(time);
  if (floored < time.to_system_time_point()) {
    floored += std::chrono::minutes{1};
  }
  return floored;
}

[[nodiscard]] core::Time cron_minute_to_time(CronMinute time) {
  return core::Time{std::chrono::time_point_cast<core::Time::clock::duration>(time)};
}

[[nodiscard]] CronMinute cron_search_start(const CronSchedule& schedule, const PeriodicJobState& state) {
  auto start = ceil_to_cron_minute(schedule.first_fire_at);
  if (state.last_fired_at.has_value()) {
    const auto after_last_fire = floor_to_cron_minute(*state.last_fired_at) + std::chrono::minutes{1};
    if (after_last_fire > start) {
      start = after_last_fire;
    }
  }
  return start;
}

[[nodiscard]] bool cron_day_matches(const ParsedCronSchedule& schedule, unsigned day_of_month, unsigned day_of_week) {
  const auto dom_match = schedule.days_of_month.test(day_of_month);
  const auto dow_match = schedule.days_of_week.test(day_of_week);
  if (schedule.day_of_month_wildcard && schedule.day_of_week_wildcard) {
    return true;
  }
  if (schedule.day_of_month_wildcard) {
    return dow_match;
  }
  if (schedule.day_of_week_wildcard) {
    return dom_match;
  }
  return dom_match || dow_match;
}

[[nodiscard]] bool cron_matches(const ParsedCronSchedule& schedule, CronMinute candidate) {
  const auto day_point = std::chrono::floor<std::chrono::days>(candidate);
  const auto ymd = std::chrono::year_month_day{day_point};
  if (!ymd.ok()) {
    return false;
  }

  const auto time_of_day = std::chrono::hh_mm_ss{candidate - day_point};
  const auto minute = static_cast<unsigned>(time_of_day.minutes().count());
  const auto hour = static_cast<unsigned>(time_of_day.hours().count());
  const auto day_of_month = static_cast<unsigned>(ymd.day());
  const auto month = static_cast<unsigned>(ymd.month());
  const auto day_of_week = static_cast<unsigned>(std::chrono::weekday{day_point}.c_encoding());

  return schedule.minutes.test(minute) && schedule.hours.test(hour) && schedule.months.test(month) &&
         cron_day_matches(schedule, day_of_month, day_of_week);
}

[[nodiscard]] core::Result<CronMinute> find_next_cron_fire(const ParsedCronSchedule& schedule, CronMinute start) {
  auto candidate = start;
  for (std::size_t checked = 0; checked <= kCronSearchLimitMinutes; ++checked) {
    if (cron_matches(schedule, candidate)) {
      return candidate;
    }
    candidate += std::chrono::minutes{1};
  }
  return std::unexpected(invalid_cron_schedule("expression", "no_matching_fire"));
}

[[nodiscard]] core::Result<void> validate_schedule(const PeriodicSchedule& schedule) {
  if (!positive_duration(schedule.interval)) {
    return std::unexpected(invalid_periodic_schedule("interval"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_retention_job(const MemoryRetentionJob& job) {
  if (job.scope_key.empty()) {
    return std::unexpected(invalid_memory_retention("scope_key"));
  }
  if (!positive_duration(job.policy.forget_after_unused)) {
    return std::unexpected(invalid_memory_retention("forget_after_unused"));
  }
  if (!std::isfinite(job.policy.importance_floor) || job.policy.importance_floor < 0.0 ||
      job.policy.importance_floor > 1.0) {
    return std::unexpected(invalid_memory_retention("importance_floor"));
  }
  if (job.policy.max_records_per_scope == 0) {
    return std::unexpected(invalid_memory_retention("max_records_per_scope"));
  }
  if (!positive_duration(job.policy.decay_check_interval)) {
    return std::unexpected(invalid_memory_retention("decay_check_interval"));
  }
  return {};
}

}  // namespace

core::Result<PeriodicEvaluation>
evaluate_periodic_schedule(PeriodicSchedule schedule, PeriodicJobState state, core::Time now) {
  if (auto valid = validate_schedule(schedule); !valid) {
    return std::unexpected(std::move(valid).error());
  }

  const auto next_fire_at =
      state.last_fired_at.has_value() ? add_duration(*state.last_fired_at, schedule.interval) : schedule.first_fire_at;
  if (now < next_fire_at) {
    return PeriodicEvaluation{
        .due = false,
        .next_fire_at = next_fire_at,
        .overdue_by = std::chrono::nanoseconds{0},
    };
  }

  return PeriodicEvaluation{
      .due = true,
      .next_fire_at = next_fire_at,
      .overdue_by = duration_since(now, next_fire_at),
  };
}

core::Result<PeriodicEvaluation> evaluate_cron_schedule(CronSchedule schedule, PeriodicJobState state, core::Time now) {
  auto parsed = parse_cron_schedule(schedule);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto next_fire = find_next_cron_fire(*parsed, cron_search_start(schedule, state));
  if (!next_fire) {
    return std::unexpected(std::move(next_fire).error());
  }

  const auto next_fire_at = cron_minute_to_time(*next_fire);
  if (now < next_fire_at) {
    return PeriodicEvaluation{
        .due = false,
        .next_fire_at = next_fire_at,
        .overdue_by = std::chrono::nanoseconds{0},
    };
  }

  return PeriodicEvaluation{
      .due = true,
      .next_fire_at = next_fire_at,
      .overdue_by = duration_since(now, next_fire_at),
  };
}

core::Result<MemoryRetentionPlan>
plan_memory_retention(MemoryRetentionJob job, PeriodicJobState state, core::Time now) {
  if (auto valid = validate_retention_job(job); !valid) {
    return std::unexpected(std::move(valid).error());
  }

  auto evaluation = evaluate_periodic_schedule(
      PeriodicSchedule{
          .first_fire_at = job.first_fire_at,
          .interval = std::chrono::duration_cast<std::chrono::nanoseconds>(job.policy.decay_check_interval),
      },
      state,
      now);
  if (!evaluation) {
    return std::unexpected(std::move(evaluation).error());
  }

  if (!evaluation->due) {
    return MemoryRetentionPlan{
        .schedule = *evaluation,
        .decay_request = std::nullopt,
    };
  }

  return MemoryRetentionPlan{
      .schedule = *evaluation,
      .decay_request =
          memory::longterm::DecayRequest{
              .scope_key = std::move(job.scope_key),
              .unused_before = core::Time{now.to_system_time_point() - job.policy.forget_after_unused},
              .importance_floor = job.policy.importance_floor,
              .limit = job.policy.max_records_per_scope,
              .decay_at = now,
          },
  };
}

}  // namespace orangutan::automation

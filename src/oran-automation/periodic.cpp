// src/oran-automation/periodic.cpp — periodic cadence and retention planning.

#include <oran/automation/periodic.hpp>

#include <chrono>
#include <cmath>
#include <expected>
#include <optional>
#include <string>

#include <oran/core/error.hpp>

namespace orangutan::automation {
namespace {

template <class Rep, class Period>
[[nodiscard]] bool positive_duration(std::chrono::duration<Rep, Period> value) noexcept {
  return value > std::chrono::duration<Rep, Period>{0};
}

[[nodiscard]] core::Error invalid_periodic_schedule(std::string field) {
  return core::Error::invalid_argument("periodic schedule is invalid").with("field", std::move(field));
}

[[nodiscard]] core::Error invalid_memory_retention(std::string field) {
  return core::Error::invalid_argument("memory retention job is invalid").with("field", std::move(field));
}

[[nodiscard]] core::Time add_duration(core::Time time, std::chrono::nanoseconds duration) {
  return core::Time{time.to_system_time_point() + duration};
}

[[nodiscard]] std::chrono::nanoseconds duration_since(core::Time later, core::Time earlier) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(later.to_system_time_point() -
                                                              earlier.to_system_time_point());
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

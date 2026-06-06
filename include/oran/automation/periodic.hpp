// include/oran/automation/periodic.hpp — deterministic periodic job planning.

#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/memory/longterm.hpp>

namespace orangutan::automation {

/// Periodic firing contract shared by future automation service owners.
/// `first_fire_at` anchors a never-fired job; after the first run, the next
/// scheduled time is `last_fired_at + interval`.
struct PeriodicSchedule {
  core::Time first_fire_at{core::Time::epoch()};
  std::chrono::nanoseconds interval{0};

  friend bool operator==(const PeriodicSchedule&, const PeriodicSchedule&) = default;
};

/// Persisted or caller-supplied state for one periodic job.
struct PeriodicJobState {
  std::optional<core::Time> last_fired_at{};

  friend bool operator==(const PeriodicJobState&, const PeriodicJobState&) = default;
};

/// Deterministic evaluation result. When `due=false`, `next_fire_at` is the
/// next future fire. When `due=true`, it is the scheduled time that should fire
/// now, and `overdue_by` reports how late the caller is.
struct PeriodicEvaluation {
  bool due{false};
  core::Time next_fire_at{core::Time::epoch()};
  std::chrono::nanoseconds overdue_by{0};

  friend bool operator==(const PeriodicEvaluation&, const PeriodicEvaluation&) = default;
};

/// Long-term memory retention policy in automation-owned units. Bootstrap can
/// map `config::LongtermMemoryRetentionConfig` into this shape without making
/// `oran-memory` depend upward on automation.
struct LongtermMemoryRetentionPolicy {
  std::chrono::days forget_after_unused{180};
  double importance_floor{0.0};
  std::size_t max_records_per_scope{10000};
  std::chrono::hours decay_check_interval{24};

  friend bool operator==(const LongtermMemoryRetentionPolicy&, const LongtermMemoryRetentionPolicy&) = default;
};

/// Periodic job descriptor for long-term retention decay. This is a planner
/// input only; it does not own a backend, hook bus, lease, or background task.
struct MemoryRetentionJob {
  std::string scope_key;
  LongtermMemoryRetentionPolicy policy{};
  core::Time first_fire_at{core::Time::epoch()};

  friend bool operator==(const MemoryRetentionJob&, const MemoryRetentionJob&) = default;
};

struct MemoryRetentionPlan {
  PeriodicEvaluation schedule{};
  std::optional<memory::longterm::DecayRequest> decay_request{};

  friend bool operator==(const MemoryRetentionPlan&, const MemoryRetentionPlan&) = default;
};

[[nodiscard]] core::Result<PeriodicEvaluation>
evaluate_periodic_schedule(PeriodicSchedule schedule, PeriodicJobState state, core::Time now);

[[nodiscard]] core::Result<MemoryRetentionPlan>
plan_memory_retention(MemoryRetentionJob job, PeriodicJobState state, core::Time now);

}  // namespace orangutan::automation

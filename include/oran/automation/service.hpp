// include/oran/automation/service.hpp - explicit automation service ticks.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/automation/repository.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::memory::longterm {
class Backend;
}  // namespace orangutan::memory::longterm

namespace orangutan::hook {
class Bus;
}  // namespace orangutan::hook

namespace orangutan::automation {

struct MemoryRetentionHookOptions {
  hook::Bus* bus{};
  std::string source{"periodic"};
  std::string agent_key{"automation"};
  std::string identity{"retention"};
};

struct MemoryRetentionServiceOptions {
  MemoryRetentionHookOptions hooks{};
};

struct MemoryRetentionTickRequest {
  std::string job_key;
  core::Time now{core::Time::epoch()};
};

struct MemoryRetentionHookPublishResult {
  std::size_t sink_count{0};
  std::size_t failure_count{0};
};

struct MemoryRetentionTickResult {
  std::string job_key;
  PeriodicEvaluation schedule{};
  bool ran{false};
  std::size_t shadowed_count{0};
  std::optional<MemoryRetentionJobRecord> job{};
  std::optional<MemoryRetentionRunRecord> run{};
  std::optional<MemoryRetentionHookPublishResult> hook_publish{};
};

struct CronTickRequest {
  core::Time now{core::Time::epoch()};
  std::size_t job_limit{100};
};

struct CronDueJob {
  CronJobRecord job{};
  PeriodicEvaluation schedule{};
};

struct CronTickResult {
  core::Time now{core::Time::epoch()};
  std::size_t checked_count{0};
  std::vector<CronDueJob> due_jobs{};
  std::optional<core::Time> next_fire_at{};
};

/// One caller-driven scan for stored cron jobs.
///
/// This owner evaluates repository-backed cron schedules and reports due work
/// plus the earliest next fire. It intentionally does not mark jobs fired,
/// publish hooks, enqueue work, call agents, or start a background loop; the
/// caller owns the execution policy and can advance state with
/// `AutomationRepository::mark_cron_job_fired(...)` after successful work.
class CronService {
public:
  explicit CronService(AutomationRepository& repository) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<CronTickResult>> tick(CronTickRequest request);
  [[nodiscard]] AutomationRepository& repository() noexcept;
  [[nodiscard]] const AutomationRepository& repository() const noexcept;

private:
  AutomationRepository* repository_{};
};

/// One caller-driven tick for the stored long-term retention job.
///
/// This owner intentionally does not start a background loop, own timers, or
/// acquire execution leases by itself. `MemoryRetentionLoop` can use the exposed
/// repository to lease due tick execution. When constructed with a hook bus, a
/// due tick publishes advisory job lifecycle metadata around backend work and
/// publishes `memory_decay` metadata after successful durable state advances.
class MemoryRetentionService {
public:
  MemoryRetentionService(AutomationRepository& repository,
                         memory::longterm::Backend& backend,
                         MemoryRetentionServiceOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<MemoryRetentionTickResult>> tick(MemoryRetentionTickRequest request);
  [[nodiscard]] AutomationRepository& repository() noexcept;
  [[nodiscard]] const AutomationRepository& repository() const noexcept;

private:
  AutomationRepository* repository_{};
  memory::longterm::Backend* backend_{};
  MemoryRetentionServiceOptions options_{};
};

}  // namespace orangutan::automation

// include/oran/automation/service.hpp - explicit automation service ticks.

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
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

struct CronHookOptions {
  hook::Bus* bus{};
  std::string source{"cron"};
  std::string agent_key{"automation"};
  std::string identity{"cron"};
};

struct CronServiceOptions {
  CronHookOptions hooks{};
};

struct TriggeredHookOptions {
  hook::Bus* bus{};
  std::string source{"triggered"};
  std::string agent_key{"automation"};
  std::string identity{"triggered"};
};

struct TriggeredServiceOptions {
  TriggeredHookOptions hooks{};
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

using CronJobHandler = std::function<async::Awaitable<core::Result<void>>(CronDueJob)>;

struct CronExecuteRequest {
  core::Time now{core::Time::epoch()};
  std::size_t job_limit{100};
  CronJobHandler handler{};
  std::string lease_owner_key{};
  std::chrono::steady_clock::duration lease_ttl{std::chrono::minutes{5}};
};

struct CronExecuteAttempt {
  CronDueJob due{};
  bool advanced{false};
  std::optional<core::Error> error{};
  std::optional<CronRunRecord> run{};
  std::optional<CronJobRecord> marked_job{};
};

struct CronExecuteResult {
  CronTickResult tick{};
  std::size_t attempted_count{0};
  std::size_t advanced_count{0};
  std::vector<CronExecuteAttempt> attempts{};
};

struct TriggeredIntakeRequest {
  std::string trigger_key;
  core::Time received_at{core::Time::epoch()};
  std::size_t job_limit{100};
};

struct TriggeredIntakeResult {
  std::string trigger_key;
  core::Time received_at{core::Time::epoch()};
  std::size_t matched_count{0};
  std::vector<TriggeredJobRecord> jobs{};
};

struct TriggeredExecutionJob {
  TriggeredJobRecord job{};
  std::string trigger_key;
  core::Time received_at{core::Time::epoch()};
};

using TriggeredJobHandler = std::function<async::Awaitable<core::Result<void>>(TriggeredExecutionJob)>;

struct TriggeredExecuteRequest {
  std::string trigger_key;
  core::Time received_at{core::Time::epoch()};
  std::size_t job_limit{100};
  TriggeredJobHandler handler{};
};

struct TriggeredExecuteAttempt {
  TriggeredExecutionJob execution{};
  bool completed{false};
  std::optional<core::Error> error{};
  std::optional<TriggeredRunRecord> run{};
};

struct TriggeredExecuteResult {
  TriggeredIntakeResult intake{};
  std::size_t attempted_count{0};
  std::size_t completed_count{0};
  std::vector<TriggeredExecuteAttempt> attempts{};
};

/// One caller-driven intake step for externally triggered jobs.
///
/// This owner matches a caller-supplied trigger key against stored triggered
/// job descriptors. `execute(...)` accepts a caller-supplied handler and records
/// one run row per matched descriptor. When constructed with a hook bus,
/// execution publishes advisory job lifecycle metadata around handler work. It
/// does not enqueue work or call agents.
class TriggeredService {
public:
  explicit TriggeredService(AutomationRepository& repository, TriggeredServiceOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<TriggeredIntakeResult>> intake(TriggeredIntakeRequest request);
  [[nodiscard]] async::Awaitable<core::Result<TriggeredExecuteResult>> execute(TriggeredExecuteRequest request);
  [[nodiscard]] AutomationRepository& repository() noexcept;
  [[nodiscard]] const AutomationRepository& repository() const noexcept;

private:
  AutomationRepository* repository_{};
  TriggeredServiceOptions options_{};
};

/// One caller-driven scan for stored cron jobs.
///
/// This owner evaluates repository-backed cron schedules and reports due work
/// plus the earliest next fire. `tick(...)` intentionally does not mark jobs
/// fired. `execute_due(...)` accepts a caller-supplied handler and advances a
/// due cron job only after that handler returns success. When constructed with
/// a hook bus, due execution publishes advisory job lifecycle metadata around
/// handler work. The service still does not enqueue work, call agents, or start
/// a background loop.
class CronService {
public:
  explicit CronService(AutomationRepository& repository, CronServiceOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<CronTickResult>> tick(CronTickRequest request);
  [[nodiscard]] async::Awaitable<core::Result<CronExecuteResult>> execute_due(CronExecuteRequest request);
  [[nodiscard]] AutomationRepository& repository() noexcept;
  [[nodiscard]] const AutomationRepository& repository() const noexcept;

private:
  AutomationRepository* repository_{};
  CronServiceOptions options_{};
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

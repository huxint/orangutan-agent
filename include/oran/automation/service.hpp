// include/oran/automation/service.hpp - explicit automation service ticks.

#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/automation/repository.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::memory::longterm {
class Backend;
}  // namespace orangutan::memory::longterm

namespace orangutan::hook {
class Bus;
}  // namespace orangutan::hook

namespace orangutan::automation {

enum class AutomationJobType : std::uint8_t {
  cron,
  triggered,
};

enum class AutomationJobOutcome : std::uint8_t {
  success,
  failure,
  aborted,
};

struct AutomationJobHandlerResult {
  std::string text{};
};

struct AutomationNotificationResult {
  bool delivered{false};
  std::optional<core::Error> error{};
};

struct AutomationJobNotification {
  std::string job_key{};
  AutomationJobType job_type{AutomationJobType::cron};
  std::string agent_key{"automation"};
  std::optional<std::string> trigger_key{};
  std::optional<std::string> trigger_payload{};
  core::Time fired_at{core::Time::epoch()};
  core::Time finished_at{core::Time::epoch()};
  AutomationJobOutcome outcome{AutomationJobOutcome::success};
  std::optional<AutomationJobHandlerResult> handler_result{};
  std::optional<std::string> error_kind{};
  std::optional<std::string> error_message{};
};

using AutomationNotifier = std::function<async::Awaitable<core::Result<void>>(AutomationJobNotification)>;

namespace detail {

template <typename Fn, typename Job>
concept RichAutomationJobHandlerFn =
    std::invocable<Fn&, Job> &&
    std::same_as<std::invoke_result_t<Fn&, Job>, async::Awaitable<core::Result<AutomationJobHandlerResult>>>;

template <typename Fn, typename Job>
concept LegacyAutomationJobHandlerFn =
    std::invocable<Fn&, Job> && std::same_as<std::invoke_result_t<Fn&, Job>, async::Awaitable<core::Result<void>>>;

template <typename Job>
class AutomationJobHandler {
public:
  AutomationJobHandler() = default;
  AutomationJobHandler(std::nullptr_t) noexcept : fn_{} {}

  template <typename Fn>
    requires(!std::same_as<std::remove_cvref_t<Fn>, AutomationJobHandler> &&
             (RichAutomationJobHandlerFn<std::remove_cvref_t<Fn>, Job> ||
              LegacyAutomationJobHandlerFn<std::remove_cvref_t<Fn>, Job>))
  AutomationJobHandler(Fn&& fn) : fn_{wrap(std::forward<Fn>(fn))} {}

  template <typename Fn>
    requires(RichAutomationJobHandlerFn<std::remove_cvref_t<Fn>, Job> ||
             LegacyAutomationJobHandlerFn<std::remove_cvref_t<Fn>, Job>)
  AutomationJobHandler& operator=(Fn&& fn) {
    fn_ = wrap(std::forward<Fn>(fn));
    return *this;
  }

  AutomationJobHandler& operator=(std::nullptr_t) noexcept {
    fn_ = nullptr;
    return *this;
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(fn_);
  }

  [[nodiscard]] async::Awaitable<core::Result<AutomationJobHandlerResult>> operator()(Job job) const {
    if (!fn_) {
      co_return std::unexpected(core::Error::invalid_argument("automation job handler is invalid")
                                    .with("field", "handler")
                                    .with("reason", "empty"));
    }
    co_return co_await fn_(std::move(job));
  }

private:
  using StorageFn = std::function<async::Awaitable<core::Result<AutomationJobHandlerResult>>(Job)>;

  template <typename Fn>
    requires RichAutomationJobHandlerFn<std::remove_cvref_t<Fn>, Job>
  static StorageFn wrap(Fn&& fn) {
    return StorageFn{std::forward<Fn>(fn)};
  }

  template <typename Fn>
    requires LegacyAutomationJobHandlerFn<std::remove_cvref_t<Fn>, Job>
  static StorageFn wrap(Fn&& fn) {
    return [fn = std::forward<Fn>(fn)](Job job) mutable -> async::Awaitable<core::Result<AutomationJobHandlerResult>> {
      auto result = co_await fn(std::move(job));
      if (!result) {
        co_return std::unexpected(std::move(result).error());
      }
      co_return AutomationJobHandlerResult{};
    };
  }

  StorageFn fn_{};
};

}  // namespace detail

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
  AutomationNotifier notifier{};
};

struct TriggeredHookOptions {
  hook::Bus* bus{};
  std::string source{"triggered"};
  std::string agent_key{"automation"};
  std::string identity{"triggered"};
};

struct TriggeredServiceOptions {
  TriggeredHookOptions hooks{};
  AutomationNotifier notifier{};
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

using CronJobHandler = detail::AutomationJobHandler<CronDueJob>;

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
  std::optional<AutomationJobHandlerResult> handler_result{};
  std::optional<CronRunRecord> run{};
  std::optional<CronJobRecord> marked_job{};
  std::optional<AutomationNotificationResult> notification{};
};

struct CronExecuteResult {
  CronTickResult tick{};
  std::size_t attempted_count{0};
  std::size_t advanced_count{0};
  std::vector<CronExecuteAttempt> attempts{};
};

struct TriggeredIntakeRequest {
  std::string trigger_key;
  std::optional<std::string> trigger_payload{};
  core::Time received_at{core::Time::epoch()};
  std::size_t job_limit{100};
};

struct TriggeredIntakeResult {
  std::string trigger_key;
  std::optional<std::string> trigger_payload{};
  core::Time received_at{core::Time::epoch()};
  std::size_t matched_count{0};
  std::vector<TriggeredJobRecord> jobs{};
};

struct TriggeredExecutionJob {
  TriggeredJobRecord job{};
  std::string trigger_key;
  std::optional<std::string> trigger_payload{};
  core::Time received_at{core::Time::epoch()};
};

using TriggeredJobHandler = detail::AutomationJobHandler<TriggeredExecutionJob>;

struct TriggeredExecuteRequest {
  std::string trigger_key;
  std::optional<std::string> trigger_payload{};
  core::Time received_at{core::Time::epoch()};
  std::size_t job_limit{100};
  TriggeredJobHandler handler{};
  std::string lease_owner_key{};
  std::chrono::steady_clock::duration lease_ttl{std::chrono::minutes{5}};
};

struct TriggeredExecuteAttempt {
  TriggeredExecutionJob execution{};
  bool completed{false};
  std::optional<core::Error> error{};
  std::optional<AutomationJobHandlerResult> handler_result{};
  std::optional<TriggeredRunRecord> run{};
  std::optional<AutomationNotificationResult> notification{};
};

struct TriggeredExecuteResult {
  TriggeredIntakeResult intake{};
  std::size_t attempted_count{0};
  std::size_t completed_count{0};
  std::vector<TriggeredExecuteAttempt> attempts{};
};

struct TriggeredExecuteOneRequest {
  TriggeredExecutionJob execution{};
  TriggeredJobHandler handler{};
  std::string lease_owner_key{};
  std::chrono::steady_clock::duration lease_ttl{std::chrono::minutes{5}};
  std::optional<core::Time> attempted_at{};
};

struct TriggeredExecuteOneResult {
  TriggeredExecuteAttempt attempt{};
  bool completed{false};
};

/// One caller-driven intake step for externally triggered jobs.
///
/// This owner matches a caller-supplied trigger key against stored triggered
/// job descriptors. `execute(...)` accepts a caller-supplied handler and records
/// one run row per matched descriptor by delegating to `execute_one(...)`, which
/// executes exactly one caller-provided descriptor. When constructed with a hook
/// bus, execution publishes advisory job lifecycle metadata around handler
/// work. When constructed with a notifier callback, execution also publishes one
/// post-outcome notification after the durable run result has been recorded,
/// carrying optional handler output text plus delivery status in the attempt
/// result. It can optionally lease the matched job's agent key before handler
/// work. It does not enqueue work or call agents.
class TriggeredService {
public:
  explicit TriggeredService(AutomationRepository& repository, TriggeredServiceOptions options = {}) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<TriggeredIntakeResult>> intake(TriggeredIntakeRequest request);
  [[nodiscard]] async::Awaitable<core::Result<TriggeredExecuteOneResult>>
  execute_one(TriggeredExecuteOneRequest request);
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
/// handler work. When constructed with a notifier callback, due execution also
/// publishes one post-outcome notification after the durable run/state
/// transition has completed, carrying optional handler output text plus
/// delivery status in the attempt result. The service still does not enqueue
/// work, call agents, or start a background loop.
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

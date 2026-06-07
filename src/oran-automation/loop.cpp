// src/oran-automation/loop.cpp - caller-started automation loop steps.

#include <oran/automation/loop.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <string>
#include <utility>

#include <asio/this_coro.hpp>

#include <oran/async/sleep.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>

namespace orangutan::automation {
namespace {

[[nodiscard]] core::Error invalid_loop_field(std::string field, std::string reason) {
  return core::Error::invalid_argument("memory retention loop field is invalid")
      .with("field", std::move(field))
      .with("reason", std::move(reason));
}

[[nodiscard]] core::Error invalid_cron_loop_field(std::string field, std::string reason) {
  return core::Error::invalid_argument("cron loop field is invalid")
      .with("field", std::move(field))
      .with("reason", std::move(reason));
}

[[nodiscard]] core::Result<void> validate_cron_run_once_request(const CronLoopRunOnceRequest& request) {
  if (request.max_wait < std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_cron_loop_field("max_wait", "negative"));
  }
  if (request.job_limit == 0) {
    return std::unexpected(invalid_cron_loop_field("job_limit", "zero"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_cron_run_request(const CronLoopRunRequest& request) {
  if (request.max_total_wait < std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_cron_loop_field("max_total_wait", "negative"));
  }
  if (request.max_iterations == 0) {
    return std::unexpected(invalid_cron_loop_field("max_iterations", "zero"));
  }
  if (request.job_limit == 0) {
    return std::unexpected(invalid_cron_loop_field("job_limit", "zero"));
  }
  if (!request.handler) {
    return std::unexpected(invalid_cron_loop_field("handler", "empty"));
  }
  if (request.lease_owner_key.empty()) {
    return std::unexpected(invalid_cron_loop_field("lease_owner_key", "empty"));
  }
  if (request.lease_ttl <= std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_cron_loop_field("lease_ttl", "not_positive"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_run_once_request(const MemoryRetentionLoopRunOnceRequest& request) {
  if (request.job_key.empty()) {
    return std::unexpected(invalid_loop_field("job_key", "empty"));
  }
  if (request.max_wait < std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_loop_field("max_wait", "negative"));
  }
  if (request.lease_owner_key.empty()) {
    return std::unexpected(invalid_loop_field("lease_owner_key", "empty"));
  }
  if (request.lease_ttl <= std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_loop_field("lease_ttl", "not_positive"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_run_request(const MemoryRetentionLoopRunRequest& request) {
  if (request.job_key.empty()) {
    return std::unexpected(invalid_loop_field("job_key", "empty"));
  }
  if (request.max_total_wait < std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_loop_field("max_total_wait", "negative"));
  }
  if (request.max_iterations == 0) {
    return std::unexpected(invalid_loop_field("max_iterations", "zero"));
  }
  if (request.lease_owner_key.empty()) {
    return std::unexpected(invalid_loop_field("lease_owner_key", "empty"));
  }
  if (request.lease_ttl <= std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_loop_field("lease_ttl", "not_positive"));
  }
  return {};
}

[[nodiscard]] std::chrono::nanoseconds wait_until(core::Time now, core::Time next_fire_at) noexcept {
  if (next_fire_at <= now) {
    return std::chrono::nanoseconds{0};
  }
  return std::chrono::duration_cast<std::chrono::nanoseconds>(next_fire_at.to_system_time_point() -
                                                              now.to_system_time_point());
}

[[nodiscard]] core::Time add_wait(core::Time now, std::chrono::nanoseconds waited_for) noexcept {
  return core::Time{now.to_system_time_point() + waited_for};
}

[[nodiscard]] core::Time add_steady_duration(core::Time now, std::chrono::steady_clock::duration duration) noexcept {
  return core::Time{now.to_system_time_point() + std::chrono::duration_cast<core::Time::clock::duration>(duration)};
}

[[nodiscard]] MemoryRetentionLoopRunOnceRequest make_run_once_request(const MemoryRetentionLoopRunRequest& request,
                                                                      core::Time now,
                                                                      std::chrono::nanoseconds remaining_wait) {
  return MemoryRetentionLoopRunOnceRequest{
      .job_key = request.job_key,
      .now = now,
      .max_wait = std::chrono::duration_cast<std::chrono::steady_clock::duration>(remaining_wait),
      .lease_owner_key = request.lease_owner_key,
      .lease_ttl = request.lease_ttl,
  };
}

[[nodiscard]] std::size_t failed_attempt_count(const CronExecuteResult& execution) noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(execution.attempts, [](const CronExecuteAttempt& attempt) {
    return attempt.error.has_value();
  }));
}

[[nodiscard]] bool should_stop(const CronLoopStopPredicate& predicate) {
  return predicate && predicate();
}

[[nodiscard]] core::Error lease_conflict_error(const MemoryRetentionLoopRunOnceRequest& request) {
  return core::Error{core::ErrorKind::conflict, "memory retention job lease is already held"}
      .with("job_key", request.job_key)
      .with("owner_key", request.lease_owner_key);
}

[[nodiscard]] core::Error lease_release_conflict_error(const MemoryRetentionLoopRunOnceRequest& request) {
  return core::Error{core::ErrorKind::conflict, "memory retention job lease was not released"}
      .with("job_key", request.job_key)
      .with("owner_key", request.lease_owner_key);
}

[[nodiscard]] core::Error attach_lease_release_error(core::Error error, const core::Error& release_error) {
  return std::move(error)
      .with("lease_release_error_kind", std::string{core::enum_name(release_error.kind())})
      .with("lease_release_error_message", std::string{release_error.message()});
}

[[nodiscard]] async::Awaitable<core::Result<void>> release_lease(MemoryRetentionService& service,
                                                                 const MemoryRetentionLoopRunOnceRequest& request) {
  co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
  auto released = co_await service.repository().release_memory_retention_lease(
      ReleaseMemoryRetentionLeaseRequest{.job_key = request.job_key, .owner_key = request.lease_owner_key});
  if (!released) {
    co_return std::unexpected(std::move(released).error());
  }
  if (!*released) {
    co_return std::unexpected(lease_release_conflict_error(request));
  }
  co_return core::Result<void>{};
}

[[nodiscard]] core::Result<MemoryRetentionTickResult> make_not_due_tick(MemoryRetentionJobRecord job,
                                                                        PeriodicEvaluation schedule) {
  return MemoryRetentionTickResult{
      .job_key = job.job_key,
      .schedule = schedule,
      .ran = false,
      .job = std::move(job),
  };
}

[[nodiscard]] async::Awaitable<core::Result<MemoryRetentionTickResult>>
tick_with_lease(MemoryRetentionService& service, const MemoryRetentionLoopRunOnceRequest& request, core::Time now) {
  auto lease = co_await service.repository().acquire_memory_retention_lease(AcquireMemoryRetentionLeaseRequest{
      .job_key = request.job_key,
      .owner_key = request.lease_owner_key,
      .acquired_at = now,
      .expires_at = add_steady_duration(now, request.lease_ttl),
  });
  if (!lease) {
    co_return std::unexpected(std::move(lease).error());
  }
  if (!lease->has_value()) {
    co_return std::unexpected(lease_conflict_error(request));
  }

  auto tick = co_await service.tick(MemoryRetentionTickRequest{
      .job_key = request.job_key,
      .now = now,
  });
  auto released = co_await release_lease(service, request);
  if (!tick) {
    auto error = std::move(tick).error();
    if (!released) {
      co_return std::unexpected(attach_lease_release_error(std::move(error), released.error()));
    }
    co_return std::unexpected(std::move(error));
  }
  if (!released) {
    co_return std::unexpected(std::move(released).error());
  }
  co_return std::move(*tick);
}

}  // namespace

CronLoop::CronLoop(asio::any_io_executor executor, CronService service) noexcept
    : executor_{std::move(executor)}, service_{std::move(service)} {}

async::Awaitable<core::Result<CronLoopRunOnceResult>> CronLoop::run_once(CronLoopRunOnceRequest request) {
  if (auto valid = validate_cron_run_once_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto tick = co_await service_.tick(CronTickRequest{.now = request.now, .job_limit = request.job_limit});
  if (!tick) {
    co_return std::unexpected(std::move(tick).error());
  }
  if (!tick->due_jobs.empty() || !tick->next_fire_at.has_value()) {
    co_return CronLoopRunOnceResult{.tick = std::move(*tick)};
  }

  const auto wait_for = wait_until(request.now, *tick->next_fire_at);
  if (wait_for > request.max_wait) {
    co_return CronLoopRunOnceResult{
        .waited_for = std::chrono::nanoseconds{0},
        .tick = std::move(*tick),
    };
  }

  auto slept = co_await async::sleep_for(executor_, wait_for);
  if (!slept) {
    co_return std::unexpected(std::move(slept).error());
  }

  auto due_tick = co_await service_.tick(CronTickRequest{
      .now = add_wait(request.now, wait_for),
      .job_limit = request.job_limit,
  });
  if (!due_tick) {
    co_return std::unexpected(std::move(due_tick).error());
  }

  co_return CronLoopRunOnceResult{
      .waited_for = wait_for,
      .tick = std::move(*due_tick),
  };
}

async::Awaitable<core::Result<CronLoopRunResult>> CronLoop::run(CronLoopRunRequest request) {
  if (auto valid = validate_cron_run_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  CronLoopRunResult result{};
  auto now = request.now;
  auto remaining_wait = std::chrono::duration_cast<std::chrono::nanoseconds>(request.max_total_wait);

  while (result.iterations < request.max_iterations) {
    if (should_stop(request.stop_requested)) {
      result.stop_reason = CronLoopRunStopReason::stop_requested;
      co_return result;
    }

    auto execution = co_await service_.execute_due(CronExecuteRequest{
        .now = now,
        .job_limit = request.job_limit,
        .handler = request.handler,
        .lease_owner_key = request.lease_owner_key,
        .lease_ttl = request.lease_ttl,
    });
    if (!execution) {
      co_return std::unexpected(std::move(execution).error());
    }

    ++result.iterations;
    result.attempted_count += execution->attempted_count;
    result.advanced_count += execution->advanced_count;
    const auto failed_count = failed_attempt_count(*execution);
    result.failed_count += failed_count;
    result.last_execution = std::move(*execution);

    if (failed_count > 0) {
      result.stop_reason = CronLoopRunStopReason::handler_failure;
      co_return result;
    }
    if (should_stop(request.stop_requested)) {
      result.stop_reason = CronLoopRunStopReason::stop_requested;
      co_return result;
    }
    if (!result.last_execution->tick.due_jobs.empty()) {
      continue;
    }
    if (!result.last_execution->tick.next_fire_at.has_value()) {
      result.stop_reason = CronLoopRunStopReason::no_due_work;
      co_return result;
    }

    const auto wait_for = wait_until(now, *result.last_execution->tick.next_fire_at);
    if (wait_for > remaining_wait) {
      result.stop_reason = CronLoopRunStopReason::no_due_work;
      co_return result;
    }

    auto slept = co_await async::sleep_for(executor_, wait_for);
    if (!slept) {
      co_return std::unexpected(std::move(slept).error());
    }

    remaining_wait -= wait_for;
    now = add_wait(now, wait_for);
    result.waited_for += wait_for;
  }

  result.stop_reason = CronLoopRunStopReason::iteration_limit;
  co_return result;
}

MemoryRetentionLoop::MemoryRetentionLoop(asio::any_io_executor executor, MemoryRetentionService service) noexcept
    : executor_{std::move(executor)}, service_{std::move(service)} {}

async::Awaitable<core::Result<MemoryRetentionLoopRunOnceResult>>
MemoryRetentionLoop::run_once(MemoryRetentionLoopRunOnceRequest request) {
  if (auto valid = validate_run_once_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto stored = co_await service_.repository().get_memory_retention_job(request.job_key);
  if (!stored) {
    co_return std::unexpected(std::move(stored).error());
  }
  if (!stored->has_value()) {
    co_return std::unexpected(
        core::Error::not_found("automation memory-retention job not found").with("job_key", request.job_key));
  }

  auto job = std::move(**stored);
  auto plan = plan_memory_retention(job.job, job.state, request.now);
  if (!plan) {
    co_return std::unexpected(std::move(plan).error());
  }
  if (plan->schedule.due) {
    auto tick = co_await tick_with_lease(service_, request, request.now);
    if (!tick) {
      co_return std::unexpected(std::move(tick).error());
    }
    co_return MemoryRetentionLoopRunOnceResult{.tick = std::move(*tick)};
  }

  const auto wait_for = wait_until(request.now, plan->schedule.next_fire_at);
  if (wait_for > request.max_wait) {
    auto tick = make_not_due_tick(std::move(job), plan->schedule);
    if (!tick) {
      co_return std::unexpected(std::move(tick).error());
    }
    co_return MemoryRetentionLoopRunOnceResult{
        .waited_for = std::chrono::nanoseconds{0},
        .tick = std::move(*tick),
    };
  }

  auto slept = co_await async::sleep_for(executor_, wait_for);
  if (!slept) {
    co_return std::unexpected(std::move(slept).error());
  }

  auto tick = co_await tick_with_lease(service_, request, add_wait(request.now, wait_for));
  if (!tick) {
    co_return std::unexpected(std::move(tick).error());
  }

  co_return MemoryRetentionLoopRunOnceResult{
      .waited_for = wait_for,
      .tick = std::move(*tick),
  };
}

async::Awaitable<core::Result<MemoryRetentionLoopRunResult>>
MemoryRetentionLoop::run(MemoryRetentionLoopRunRequest request) {
  if (auto valid = validate_run_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  MemoryRetentionLoopRunResult result{};
  auto now = request.now;
  auto remaining_wait = std::chrono::duration_cast<std::chrono::nanoseconds>(request.max_total_wait);

  while (result.iterations < request.max_iterations) {
    auto step = co_await run_once(make_run_once_request(request, now, remaining_wait));
    if (!step) {
      co_return std::unexpected(std::move(step).error());
    }

    const auto waited_for = step->waited_for;
    if (waited_for > remaining_wait) {
      co_return std::unexpected(
          core::Error::internal("memory retention loop exceeded wait budget").with("job_key", request.job_key));
    }

    remaining_wait -= waited_for;
    now = add_wait(now, waited_for);
    ++result.iterations;
    result.waited_for += waited_for;
    if (step->tick.ran) {
      ++result.due_runs;
    }
    result.last_step = std::move(*step);

    if (!result.last_step->tick.ran) {
      result.stop_reason = MemoryRetentionLoopRunStopReason::no_due_work;
      co_return result;
    }
  }

  result.stop_reason = MemoryRetentionLoopRunStopReason::iteration_limit;
  co_return result;
}

}  // namespace orangutan::automation

// src/oran-automation/service.cpp - caller-driven automation service ticks.

#include <oran/automation/service.hpp>

#include <chrono>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <asio/this_coro.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/hook/bus.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/memory/longterm.hpp>

namespace orangutan::automation {
namespace {

[[nodiscard]] core::Error invalid_tick_field(std::string field, std::string reason) {
  return core::Error::invalid_argument("memory retention tick field is invalid")
      .with("field", std::move(field))
      .with("reason", std::move(reason));
}

[[nodiscard]] core::Error invalid_cron_tick_field(std::string field, std::string reason) {
  return core::Error::invalid_argument("cron tick field is invalid")
      .with("field", std::move(field))
      .with("reason", std::move(reason));
}

[[nodiscard]] core::Error invalid_cron_execute_field(std::string field, std::string reason) {
  return core::Error::invalid_argument("cron execute field is invalid")
      .with("field", std::move(field))
      .with("reason", std::move(reason));
}

[[nodiscard]] core::Error invalid_triggered_intake_field(std::string field, std::string reason) {
  return core::Error::invalid_argument("triggered intake field is invalid")
      .with("field", std::move(field))
      .with("reason", std::move(reason));
}

[[nodiscard]] core::Error invalid_triggered_execute_field(std::string field, std::string reason) {
  return core::Error::invalid_argument("triggered execute field is invalid")
      .with("field", std::move(field))
      .with("reason", std::move(reason));
}

[[nodiscard]] core::Result<void> validate_tick_request(const MemoryRetentionTickRequest& request) {
  if (request.job_key.empty()) {
    return std::unexpected(invalid_tick_field("job_key", "empty"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_cron_tick_request(const CronTickRequest& request) {
  if (request.job_limit == 0) {
    return std::unexpected(invalid_cron_tick_field("job_limit", "zero"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_cron_execute_request(const CronExecuteRequest& request) {
  if (request.job_limit == 0) {
    return std::unexpected(invalid_cron_execute_field("job_limit", "zero"));
  }
  if (!request.handler) {
    return std::unexpected(invalid_cron_execute_field("handler", "empty"));
  }
  if (!request.lease_owner_key.empty() && request.lease_ttl <= std::chrono::steady_clock::duration::zero()) {
    return std::unexpected(invalid_cron_execute_field("lease_ttl", "not_positive"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_triggered_intake_request(const TriggeredIntakeRequest& request) {
  if (request.trigger_key.empty()) {
    return std::unexpected(invalid_triggered_intake_field("trigger_key", "empty"));
  }
  if (request.job_limit == 0) {
    return std::unexpected(invalid_triggered_intake_field("job_limit", "zero"));
  }
  return {};
}

[[nodiscard]] core::Result<void> validate_triggered_execute_request(const TriggeredExecuteRequest& request) {
  if (request.trigger_key.empty()) {
    return std::unexpected(invalid_triggered_execute_field("trigger_key", "empty"));
  }
  if (request.job_limit == 0) {
    return std::unexpected(invalid_triggered_execute_field("job_limit", "zero"));
  }
  if (!request.handler) {
    return std::unexpected(invalid_triggered_execute_field("handler", "empty"));
  }
  return {};
}

void track_next_fire(CronTickResult& result, core::Time next_fire_at) {
  if (!result.next_fire_at.has_value() || next_fire_at < *result.next_fire_at) {
    result.next_fire_at = next_fire_at;
  }
}

[[nodiscard]] std::string failure_message(const core::Error& error, std::string_view fallback) {
  if (error.message().empty()) {
    return std::string{fallback};
  }
  return std::string{error.message()};
}

[[nodiscard]] CronRunOutcome cron_run_outcome_for_error(const core::Error& error) noexcept {
  return error.kind() == core::ErrorKind::cancelled ? CronRunOutcome::aborted : CronRunOutcome::failure;
}

[[nodiscard]] TriggeredRunOutcome triggered_run_outcome_for_error(const core::Error& error) noexcept {
  return error.kind() == core::ErrorKind::cancelled ? TriggeredRunOutcome::aborted : TriggeredRunOutcome::failure;
}

[[nodiscard]] core::Time add_steady_duration(core::Time now, std::chrono::steady_clock::duration duration) noexcept {
  return core::Time{now.to_system_time_point() + std::chrono::duration_cast<core::Time::clock::duration>(duration)};
}

[[nodiscard]] core::Error cron_lease_conflict_error(const CronDueJob& due, std::string_view owner_key) {
  return core::Error{core::ErrorKind::conflict, "cron job lease is already held"}
      .with("job_key", due.job.job_key)
      .with("owner_key", std::string{owner_key});
}

[[nodiscard]] core::Error cron_agent_lease_conflict_error(const CronDueJob& due, std::string_view owner_key) {
  return core::Error{core::ErrorKind::conflict, "cron agent lease is already held"}
      .with("job_key", due.job.job_key)
      .with("agent_key", due.job.agent_key)
      .with("owner_key", std::string{owner_key});
}

[[nodiscard]] core::Error cron_lease_release_conflict_error(const CronDueJob& due, std::string_view owner_key) {
  return core::Error{core::ErrorKind::conflict, "cron job lease was not released"}
      .with("job_key", due.job.job_key)
      .with("owner_key", std::string{owner_key});
}

[[nodiscard]] core::Error cron_agent_lease_release_conflict_error(const CronDueJob& due, std::string_view owner_key) {
  return core::Error{core::ErrorKind::conflict, "cron agent lease was not released"}
      .with("job_key", due.job.job_key)
      .with("agent_key", due.job.agent_key)
      .with("owner_key", std::string{owner_key});
}

[[nodiscard]] core::Error attach_cron_lease_release_error(core::Error error, const core::Error& release_error) {
  return std::move(error)
      .with("lease_release_error_kind", std::string{core::enum_name(release_error.kind())})
      .with("lease_release_error_message", std::string{release_error.message()});
}

void remember_lease_release_error(std::optional<core::Error>& first_error, core::Error error) {
  if (!first_error.has_value()) {
    first_error = std::move(error);
    return;
  }
  first_error->with("additional_lease_release_error_kind", std::string{core::enum_name(error.kind())})
      .with("additional_lease_release_error_message", std::string{error.message()});
}

[[nodiscard]] async::Awaitable<core::Result<void>> release_cron_execution_leases(AutomationRepository& repository,
                                                                                 const CronDueJob& due,
                                                                                 std::string_view owner_key,
                                                                                 bool release_agent_lease) {
  co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
  auto first_error = std::optional<core::Error>{};
  if (release_agent_lease) {
    auto released = co_await repository.release_cron_agent_lease(ReleaseCronAgentLeaseRequest{
        .agent_key = due.job.agent_key,
        .owner_key = std::string{owner_key},
    });
    if (!released) {
      remember_lease_release_error(first_error, std::move(released).error());
    } else if (!*released) {
      remember_lease_release_error(first_error, cron_agent_lease_release_conflict_error(due, owner_key));
    }
  }

  auto released = co_await repository.release_cron_lease(ReleaseCronLeaseRequest{
      .job_key = due.job.job_key,
      .owner_key = std::string{owner_key},
  });
  if (!released) {
    remember_lease_release_error(first_error, std::move(released).error());
  } else if (!*released) {
    remember_lease_release_error(first_error, cron_lease_release_conflict_error(due, owner_key));
  }

  if (first_error.has_value()) {
    co_return std::unexpected(std::move(*first_error));
  }
  co_return core::Result<void>{};
}

[[nodiscard]] std::chrono::nanoseconds duration_between(core::Time started_at, core::Time finished_at) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(finished_at.to_system_time_point() -
                                                              started_at.to_system_time_point());
}

[[nodiscard]] hook::MemoryDecayPayload make_memory_decay_payload(const MemoryRetentionHookOptions& hooks,
                                                                 const MemoryRetentionJobRecord& job,
                                                                 const memory::longterm::DecayRequest& request,
                                                                 std::size_t shadowed_count) {
  return hook::MemoryDecayPayload{
      .who =
          hook::Identity{
              .scope_key = job.job.scope_key,
              .agent_key = hooks.agent_key,
              .identity = hooks.identity,
          },
      .source = hooks.source,
      .scope_key = job.job.scope_key,
      .unused_before = request.unused_before,
      .importance_floor = request.importance_floor,
      .limit = request.limit,
      .decay_at = request.decay_at,
      .shadowed_count = shadowed_count,
      .started_at = request.decay_at,
      .finished_at = request.decay_at,
      .duration = std::chrono::nanoseconds{0},
  };
}

[[nodiscard]] hook::JobLifecyclePayload make_job_started_payload(const MemoryRetentionHookOptions& hooks,
                                                                 const MemoryRetentionJobRecord& job,
                                                                 const PeriodicEvaluation& schedule,
                                                                 core::Time started_at) {
  hook::JobLifecyclePayload payload;
  payload.who = hook::Identity{
      .scope_key = job.job.scope_key,
      .agent_key = hooks.agent_key,
      .identity = hooks.identity,
  };
  payload.source = hooks.source;
  payload.job_key = job.job_key;
  payload.job_type = "memory_retention";
  payload.scope_key = job.job.scope_key;
  payload.scheduled_at = schedule.next_fire_at;
  payload.started_at = started_at;
  return payload;
}

[[nodiscard]] hook::JobLifecyclePayload make_job_finished_payload(const MemoryRetentionHookOptions& hooks,
                                                                  const MemoryRetentionJobRecord& job,
                                                                  const PeriodicEvaluation& schedule,
                                                                  core::Time started_at,
                                                                  core::Time finished_at,
                                                                  std::size_t shadowed_count) {
  auto payload = make_job_started_payload(hooks, job, schedule, started_at);
  payload.finished_at = finished_at;
  payload.duration = duration_between(started_at, finished_at);
  payload.succeeded = true;
  payload.shadowed_count = shadowed_count;
  return payload;
}

[[nodiscard]] hook::JobLifecyclePayload make_job_failed_payload(const MemoryRetentionHookOptions& hooks,
                                                                const MemoryRetentionJobRecord& job,
                                                                const PeriodicEvaluation& schedule,
                                                                core::Time started_at,
                                                                core::Time finished_at,
                                                                const core::Error& error) {
  auto payload = make_job_started_payload(hooks, job, schedule, started_at);
  payload.finished_at = finished_at;
  payload.duration = duration_between(started_at, finished_at);
  payload.error_kind = std::string{core::enum_name(error.kind())};
  payload.error_message = failure_message(error, "memory retention decay failed");
  return payload;
}

[[nodiscard]] hook::JobLifecyclePayload
make_cron_job_started_payload(const CronHookOptions& hooks, const CronDueJob& due, core::Time started_at) {
  hook::JobLifecyclePayload payload;
  payload.who = hook::Identity{
      .scope_key = {},
      .agent_key = due.job.agent_key,
      .identity = hooks.identity,
  };
  payload.source = hooks.source;
  payload.job_key = due.job.job_key;
  payload.job_type = "cron";
  payload.scheduled_at = due.schedule.next_fire_at;
  payload.started_at = started_at;
  return payload;
}

[[nodiscard]] hook::JobLifecyclePayload make_cron_job_finished_payload(const CronHookOptions& hooks,
                                                                       const CronDueJob& due,
                                                                       core::Time started_at,
                                                                       core::Time finished_at) {
  auto payload = make_cron_job_started_payload(hooks, due, started_at);
  payload.finished_at = finished_at;
  payload.duration = duration_between(started_at, finished_at);
  payload.succeeded = true;
  return payload;
}

[[nodiscard]] hook::JobLifecyclePayload make_cron_job_failed_payload(const CronHookOptions& hooks,
                                                                     const CronDueJob& due,
                                                                     core::Time started_at,
                                                                     core::Time finished_at,
                                                                     const core::Error& error) {
  auto payload = make_cron_job_started_payload(hooks, due, started_at);
  payload.finished_at = finished_at;
  payload.duration = duration_between(started_at, finished_at);
  payload.error_kind = std::string{core::enum_name(error.kind())};
  payload.error_message = failure_message(error, "cron job handler failed");
  return payload;
}

[[nodiscard]] hook::JobLifecyclePayload make_triggered_job_started_payload(const TriggeredHookOptions& hooks,
                                                                           const TriggeredExecutionJob& execution,
                                                                           core::Time started_at) {
  hook::JobLifecyclePayload payload;
  payload.who = hook::Identity{
      .scope_key = {},
      .agent_key = execution.job.agent_key,
      .identity = hooks.identity,
  };
  payload.source = hooks.source;
  payload.job_key = execution.job.job_key;
  payload.job_type = "triggered";
  payload.scheduled_at = execution.received_at;
  payload.started_at = started_at;
  return payload;
}

[[nodiscard]] hook::JobLifecyclePayload make_triggered_job_finished_payload(const TriggeredHookOptions& hooks,
                                                                            const TriggeredExecutionJob& execution,
                                                                            core::Time started_at,
                                                                            core::Time finished_at) {
  auto payload = make_triggered_job_started_payload(hooks, execution, started_at);
  payload.finished_at = finished_at;
  payload.duration = duration_between(started_at, finished_at);
  payload.succeeded = true;
  return payload;
}

[[nodiscard]] hook::JobLifecyclePayload make_triggered_job_failed_payload(const TriggeredHookOptions& hooks,
                                                                          const TriggeredExecutionJob& execution,
                                                                          core::Time started_at,
                                                                          core::Time finished_at,
                                                                          const core::Error& error) {
  auto payload = make_triggered_job_started_payload(hooks, execution, started_at);
  payload.finished_at = finished_at;
  payload.duration = duration_between(started_at, finished_at);
  payload.error_kind = std::string{core::enum_name(error.kind())};
  payload.error_message = failure_message(error, "triggered job handler failed");
  return payload;
}

[[nodiscard]] async::Awaitable<std::optional<MemoryRetentionHookPublishResult>>
publish_memory_decay(const MemoryRetentionHookOptions& hooks,
                     const MemoryRetentionJobRecord& job,
                     const memory::longterm::DecayRequest& request,
                     std::size_t shadowed_count) {
  if (hooks.bus == nullptr) {
    co_return std::nullopt;
  }
  auto outcome = co_await hooks.bus->publish_advisory(hook::Event::memory_decay,
                                                      make_memory_decay_payload(hooks, job, request, shadowed_count));
  co_return MemoryRetentionHookPublishResult{
      .sink_count = outcome.sinks.size(),
      .failure_count = outcome.failure_count(),
  };
}

async::Awaitable<void>
publish_job_lifecycle(const MemoryRetentionHookOptions& hooks, hook::Event event, hook::JobLifecyclePayload payload) {
  if (hooks.bus == nullptr) {
    co_return;
  }
  [[maybe_unused]] auto outcome = co_await hooks.bus->publish_advisory(event, std::move(payload));
}

async::Awaitable<void>
publish_job_lifecycle(const CronHookOptions& hooks, hook::Event event, hook::JobLifecyclePayload payload) {
  if (hooks.bus == nullptr) {
    co_return;
  }
  [[maybe_unused]] auto outcome = co_await hooks.bus->publish_advisory(event, std::move(payload));
}

async::Awaitable<void>
publish_job_lifecycle(const TriggeredHookOptions& hooks, hook::Event event, hook::JobLifecyclePayload payload) {
  if (hooks.bus == nullptr) {
    co_return;
  }
  [[maybe_unused]] auto outcome = co_await hooks.bus->publish_advisory(event, std::move(payload));
}

}  // namespace

TriggeredService::TriggeredService(AutomationRepository& repository, TriggeredServiceOptions options) noexcept
    : repository_{&repository}, options_{std::move(options)} {}

AutomationRepository& TriggeredService::repository() noexcept {
  return *repository_;
}

const AutomationRepository& TriggeredService::repository() const noexcept {
  return *repository_;
}

async::Awaitable<core::Result<TriggeredIntakeResult>> TriggeredService::intake(TriggeredIntakeRequest request) {
  if (auto valid = validate_triggered_intake_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto jobs = co_await repository_->list_triggered_jobs(ListTriggeredJobsOptions{
      .trigger_key = request.trigger_key,
      .limit = request.job_limit,
  });
  if (!jobs) {
    co_return std::unexpected(std::move(jobs).error());
  }

  auto matched_count = jobs->size();
  co_return TriggeredIntakeResult{
      .trigger_key = std::move(request.trigger_key),
      .received_at = request.received_at,
      .matched_count = matched_count,
      .jobs = std::move(*jobs),
  };
}

async::Awaitable<core::Result<TriggeredExecuteResult>> TriggeredService::execute(TriggeredExecuteRequest request) {
  if (auto valid = validate_triggered_execute_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto intake = co_await this->intake(TriggeredIntakeRequest{
      .trigger_key = request.trigger_key,
      .received_at = request.received_at,
      .job_limit = request.job_limit,
  });
  if (!intake) {
    co_return std::unexpected(std::move(intake).error());
  }

  TriggeredExecuteResult result{.intake = std::move(*intake)};
  for (const auto& job : result.intake.jobs) {
    TriggeredExecutionJob execution{
        .job = job,
        .trigger_key = result.intake.trigger_key,
        .received_at = result.intake.received_at,
    };
    TriggeredExecuteAttempt attempt{.execution = execution};
    ++result.attempted_count;
    const auto started_at = result.intake.received_at;

    co_await publish_job_lifecycle(options_.hooks,
                                   hook::Event::job_started,
                                   make_triggered_job_started_payload(options_.hooks, execution, started_at));

    auto executed = co_await request.handler(execution);
    if (!executed) {
      auto error = std::move(executed).error();
      auto recorded = co_await repository_->record_triggered_run(RecordTriggeredRunRequest{
          .job_key = job.job_key,
          .trigger_key = result.intake.trigger_key,
          .fired_at = result.intake.received_at,
          .finished_at = result.intake.received_at,
          .outcome = triggered_run_outcome_for_error(error),
          .error_message = failure_message(error, "triggered job handler failed"),
      });
      if (!recorded) {
        co_return std::unexpected(std::move(recorded).error());
      }
      co_await publish_job_lifecycle(
          options_.hooks,
          hook::Event::job_failed,
          make_triggered_job_failed_payload(options_.hooks, execution, started_at, result.intake.received_at, error));
      attempt.error = std::move(error);
      attempt.run = std::move(*recorded);
      result.attempts.push_back(std::move(attempt));
      continue;
    }

    auto recorded = co_await repository_->record_triggered_run(RecordTriggeredRunRequest{
        .job_key = job.job_key,
        .trigger_key = result.intake.trigger_key,
        .fired_at = result.intake.received_at,
        .finished_at = result.intake.received_at,
        .outcome = TriggeredRunOutcome::success,
    });
    if (!recorded) {
      co_return std::unexpected(std::move(recorded).error());
    }

    attempt.completed = true;
    attempt.run = std::move(*recorded);
    ++result.completed_count;
    co_await publish_job_lifecycle(
        options_.hooks,
        hook::Event::job_finished,
        make_triggered_job_finished_payload(options_.hooks, execution, started_at, result.intake.received_at));
    result.attempts.push_back(std::move(attempt));
  }

  co_return result;
}

CronService::CronService(AutomationRepository& repository, CronServiceOptions options) noexcept
    : repository_{&repository}, options_{std::move(options)} {}

AutomationRepository& CronService::repository() noexcept {
  return *repository_;
}

const AutomationRepository& CronService::repository() const noexcept {
  return *repository_;
}

async::Awaitable<core::Result<CronTickResult>> CronService::tick(CronTickRequest request) {
  if (auto valid = validate_cron_tick_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto jobs = co_await repository_->list_cron_jobs(ListCronJobsOptions{.limit = request.job_limit});
  if (!jobs) {
    co_return std::unexpected(std::move(jobs).error());
  }

  CronTickResult result{
      .now = request.now,
      .checked_count = jobs->size(),
  };
  for (auto& job : *jobs) {
    auto schedule = evaluate_cron_schedule(job.schedule, job.state, request.now);
    if (!schedule) {
      co_return std::unexpected(std::move(schedule).error().with("job_key", job.job_key));
    }
    track_next_fire(result, schedule->next_fire_at);
    if (schedule->due) {
      result.due_jobs.push_back(CronDueJob{
          .job = std::move(job),
          .schedule = *schedule,
      });
    }
  }

  co_return result;
}

async::Awaitable<core::Result<CronExecuteResult>> CronService::execute_due(CronExecuteRequest request) {
  if (auto valid = validate_cron_execute_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto tick = co_await this->tick(CronTickRequest{.now = request.now, .job_limit = request.job_limit});
  if (!tick) {
    co_return std::unexpected(std::move(tick).error());
  }

  CronExecuteResult result{.tick = std::move(*tick)};
  for (const auto& due : result.tick.due_jobs) {
    const auto lease_enabled = !request.lease_owner_key.empty();
    auto agent_lease_acquired = false;
    if (lease_enabled) {
      auto lease = co_await repository_->acquire_cron_lease(AcquireCronLeaseRequest{
          .job_key = due.job.job_key,
          .owner_key = request.lease_owner_key,
          .acquired_at = request.now,
          .expires_at = add_steady_duration(request.now, request.lease_ttl),
      });
      if (!lease) {
        co_return std::unexpected(std::move(lease).error());
      }
      if (!lease->has_value()) {
        co_return std::unexpected(cron_lease_conflict_error(due, request.lease_owner_key));
      }

      auto agent_lease = co_await repository_->acquire_cron_agent_lease(AcquireCronAgentLeaseRequest{
          .agent_key = due.job.agent_key,
          .owner_key = request.lease_owner_key,
          .acquired_at = request.now,
          .expires_at = add_steady_duration(request.now, request.lease_ttl),
      });
      if (!agent_lease) {
        auto released = co_await release_cron_execution_leases(*repository_, due, request.lease_owner_key, false);
        if (!released) {
          co_return std::unexpected(attach_cron_lease_release_error(std::move(agent_lease).error(), released.error()));
        }
        co_return std::unexpected(std::move(agent_lease).error());
      }
      if (!agent_lease->has_value()) {
        auto conflict = cron_agent_lease_conflict_error(due, request.lease_owner_key);
        auto released = co_await release_cron_execution_leases(*repository_, due, request.lease_owner_key, false);
        if (!released) {
          co_return std::unexpected(attach_cron_lease_release_error(std::move(conflict), released.error()));
        }
        co_return std::unexpected(std::move(conflict));
      }
      agent_lease_acquired = true;
    }

    CronExecuteAttempt attempt{.due = due};
    ++result.attempted_count;
    const auto started_at = request.now;

    co_await publish_job_lifecycle(options_.hooks,
                                   hook::Event::job_started,
                                   make_cron_job_started_payload(options_.hooks, due, started_at));

    auto executed = co_await request.handler(due);
    if (!executed) {
      auto error = std::move(executed).error();
      auto recorded = co_await repository_->record_cron_run(RecordCronRunRequest{
          .job_key = due.job.job_key,
          .fired_at = due.schedule.next_fire_at,
          .finished_at = request.now,
          .outcome = cron_run_outcome_for_error(error),
          .error_message = failure_message(error, "cron job handler failed"),
      });
      if (!recorded) {
        if (lease_enabled) {
          auto released =
              co_await release_cron_execution_leases(*repository_, due, request.lease_owner_key, agent_lease_acquired);
          if (!released) {
            co_return std::unexpected(attach_cron_lease_release_error(std::move(recorded).error(), released.error()));
          }
        }
        co_return std::unexpected(std::move(recorded).error());
      }
      if (lease_enabled) {
        auto released =
            co_await release_cron_execution_leases(*repository_, due, request.lease_owner_key, agent_lease_acquired);
        if (!released) {
          co_return std::unexpected(std::move(released).error());
        }
      }
      co_await publish_job_lifecycle(options_.hooks,
                                     hook::Event::job_failed,
                                     make_cron_job_failed_payload(options_.hooks, due, started_at, request.now, error));
      attempt.error = std::move(error);
      attempt.run = std::move(*recorded);
      result.attempts.push_back(std::move(attempt));
      continue;
    }

    auto recorded = co_await repository_->record_cron_run(RecordCronRunRequest{
        .job_key = due.job.job_key,
        .fired_at = due.schedule.next_fire_at,
        .finished_at = request.now,
        .outcome = CronRunOutcome::success,
    });
    if (!recorded) {
      if (lease_enabled) {
        auto released =
            co_await release_cron_execution_leases(*repository_, due, request.lease_owner_key, agent_lease_acquired);
        if (!released) {
          co_return std::unexpected(attach_cron_lease_release_error(std::move(recorded).error(), released.error()));
        }
      }
      co_return std::unexpected(std::move(recorded).error());
    }

    auto marked = co_await repository_->mark_cron_job_fired(due.job.job_key, due.schedule.next_fire_at);
    if (!marked) {
      if (lease_enabled) {
        auto released =
            co_await release_cron_execution_leases(*repository_, due, request.lease_owner_key, agent_lease_acquired);
        if (!released) {
          co_return std::unexpected(
              attach_cron_lease_release_error(std::move(marked).error().with("job_key", due.job.job_key),
                                              released.error()));
        }
      }
      co_return std::unexpected(std::move(marked).error().with("job_key", due.job.job_key));
    }
    if (lease_enabled) {
      auto released =
          co_await release_cron_execution_leases(*repository_, due, request.lease_owner_key, agent_lease_acquired);
      if (!released) {
        co_return std::unexpected(std::move(released).error());
      }
    }

    attempt.advanced = true;
    attempt.run = std::move(*recorded);
    attempt.marked_job = std::move(*marked);
    ++result.advanced_count;
    co_await publish_job_lifecycle(options_.hooks,
                                   hook::Event::job_finished,
                                   make_cron_job_finished_payload(options_.hooks, due, started_at, request.now));
    result.attempts.push_back(std::move(attempt));
  }

  co_return result;
}

MemoryRetentionService::MemoryRetentionService(AutomationRepository& repository,
                                               memory::longterm::Backend& backend,
                                               MemoryRetentionServiceOptions options) noexcept
    : repository_{&repository}, backend_{&backend}, options_{std::move(options)} {}

AutomationRepository& MemoryRetentionService::repository() noexcept {
  return *repository_;
}

const AutomationRepository& MemoryRetentionService::repository() const noexcept {
  return *repository_;
}

async::Awaitable<core::Result<MemoryRetentionTickResult>>
MemoryRetentionService::tick(MemoryRetentionTickRequest request) {
  if (auto valid = validate_tick_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto stored = co_await repository_->get_memory_retention_job(request.job_key);
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
  if (!plan->schedule.due) {
    co_return MemoryRetentionTickResult{
        .job_key = job.job_key,
        .schedule = plan->schedule,
        .ran = false,
        .job = std::move(job),
    };
  }

  const auto started_at = request.now;
  co_await publish_job_lifecycle(options_.hooks,
                                 hook::Event::job_started,
                                 make_job_started_payload(options_.hooks, job, plan->schedule, started_at));

  auto decayed = co_await backend_->decay(*plan->decay_request);
  if (!decayed) {
    auto backend_error = std::move(decayed).error();
    auto recorded = co_await repository_->record_memory_retention_run(RecordMemoryRetentionRunRequest{
        .job_key = job.job_key,
        .fired_at = plan->schedule.next_fire_at,
        .finished_at = request.now,
        .success = false,
        .shadowed_count = 0,
        .error_message = failure_message(backend_error, "memory retention decay failed"),
    });
    if (!recorded) {
      co_return std::unexpected(std::move(recorded).error());
    }
    co_await publish_job_lifecycle(
        options_.hooks,
        hook::Event::job_failed,
        make_job_failed_payload(options_.hooks, job, plan->schedule, started_at, request.now, backend_error));
    co_return std::unexpected(std::move(backend_error));
  }

  auto recorded = co_await repository_->record_memory_retention_run(RecordMemoryRetentionRunRequest{
      .job_key = job.job_key,
      .fired_at = plan->schedule.next_fire_at,
      .finished_at = request.now,
      .success = true,
      .shadowed_count = decayed->shadowed_records.size(),
  });
  if (!recorded) {
    co_return std::unexpected(std::move(recorded).error());
  }

  auto advanced = co_await repository_->mark_memory_retention_fired(job.job_key, plan->schedule.next_fire_at);
  if (!advanced) {
    co_return std::unexpected(std::move(advanced).error());
  }
  co_await publish_job_lifecycle(options_.hooks,
                                 hook::Event::job_finished,
                                 make_job_finished_payload(options_.hooks,
                                                           *advanced,
                                                           plan->schedule,
                                                           started_at,
                                                           request.now,
                                                           decayed->shadowed_records.size()));
  auto hook_publish =
      co_await publish_memory_decay(options_.hooks, *advanced, *plan->decay_request, decayed->shadowed_records.size());

  co_return MemoryRetentionTickResult{
      .job_key = job.job_key,
      .schedule = plan->schedule,
      .ran = true,
      .shadowed_count = decayed->shadowed_records.size(),
      .job = std::move(*advanced),
      .run = std::move(*recorded),
      .hook_publish = std::move(hook_publish),
  };
}

}  // namespace orangutan::automation

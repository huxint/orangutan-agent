// src/oran-automation/service.cpp - caller-driven automation service ticks.

#include <oran/automation/service.hpp>

#include <chrono>
#include <expected>
#include <string>
#include <utility>

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

[[nodiscard]] core::Result<void> validate_tick_request(const MemoryRetentionTickRequest& request) {
  if (request.job_key.empty()) {
    return std::unexpected(invalid_tick_field("job_key", "empty"));
  }
  return {};
}

[[nodiscard]] std::string failure_message(const core::Error& error) {
  if (error.message().empty()) {
    return "memory retention decay failed";
  }
  return std::string{error.message()};
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
  payload.error_message = failure_message(error);
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

}  // namespace

MemoryRetentionService::MemoryRetentionService(AutomationRepository& repository,
                                               memory::longterm::Backend& backend,
                                               MemoryRetentionServiceOptions options) noexcept
    : repository_{&repository}, backend_{&backend}, options_{std::move(options)} {}

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
        .error_message = failure_message(backend_error),
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

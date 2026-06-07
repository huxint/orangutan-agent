// src/oran-automation/service.cpp - caller-driven automation service ticks.

#include <oran/automation/service.hpp>

#include <expected>
#include <string>
#include <utility>

#include <oran/core/error.hpp>
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

}  // namespace

MemoryRetentionService::MemoryRetentionService(AutomationRepository& repository,
                                               memory::longterm::Backend& backend) noexcept
    : repository_{&repository}, backend_{&backend} {}

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

  co_return MemoryRetentionTickResult{
      .job_key = job.job_key,
      .schedule = plan->schedule,
      .ran = true,
      .shadowed_count = decayed->shadowed_records.size(),
      .job = std::move(*advanced),
      .run = std::move(*recorded),
  };
}

}  // namespace orangutan::automation

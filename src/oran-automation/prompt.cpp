// src/oran-automation/prompt.cpp - prompt-runner adapters for automation jobs.

#include <oran/automation/prompt.hpp>

#include <expected>
#include <utility>

#include <oran/core/error.hpp>

namespace orangutan::automation {
namespace {

[[nodiscard]] core::Error invalid_prompt_runner() {
  return core::Error::invalid_argument("automation prompt runner is invalid")
      .with("field", "runner")
      .with("reason", "empty");
}

}  // namespace

AutomationPromptRunRequest make_cron_prompt_run_request(const CronDueJob& due) {
  return AutomationPromptRunRequest{
      .job_key = due.job.job_key,
      .job_type = AutomationPromptJobType::cron,
      .agent_key = due.job.agent_key,
      .prompt = due.job.agent_prompt,
      .fired_at = due.schedule.next_fire_at,
  };
}

AutomationPromptRunRequest make_triggered_prompt_run_request(const TriggeredExecutionJob& execution) {
  return AutomationPromptRunRequest{
      .job_key = execution.job.job_key,
      .job_type = AutomationPromptJobType::triggered,
      .agent_key = execution.job.agent_key,
      .prompt = execution.job.agent_prompt,
      .fired_at = execution.received_at,
      .trigger_key = execution.trigger_key,
      .trigger_payload = execution.trigger_payload,
  };
}

CronJobHandler make_cron_prompt_handler(AutomationPromptRunner runner) {
  return [runner = std::move(runner)](CronDueJob due) -> async::Awaitable<core::Result<AutomationJobHandlerResult>> {
    if (!runner) {
      co_return std::unexpected(invalid_prompt_runner());
    }
    auto result = co_await runner(make_cron_prompt_run_request(due));
    if (!result) {
      co_return std::unexpected(std::move(result).error());
    }
    co_return AutomationJobHandlerResult{.text = std::move(result->text)};
  };
}

TriggeredJobHandler make_triggered_prompt_handler(AutomationPromptRunner runner) {
  return [runner = std::move(runner)](
             TriggeredExecutionJob execution) -> async::Awaitable<core::Result<AutomationJobHandlerResult>> {
    if (!runner) {
      co_return std::unexpected(invalid_prompt_runner());
    }
    auto result = co_await runner(make_triggered_prompt_run_request(execution));
    if (!result) {
      co_return std::unexpected(std::move(result).error());
    }
    co_return AutomationJobHandlerResult{.text = std::move(result->text)};
  };
}

}  // namespace orangutan::automation

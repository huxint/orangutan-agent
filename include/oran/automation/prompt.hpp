// include/oran/automation/prompt.hpp - prompt-runner adapters for automation jobs.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/automation/service.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

namespace orangutan::automation {

enum class AutomationPromptJobType : std::uint8_t {
  cron,
  triggered,
};

struct AutomationPromptRunRequest {
  std::string job_key;
  AutomationPromptJobType job_type{AutomationPromptJobType::cron};
  std::string agent_key{"automation"};
  std::string prompt;
  core::Time fired_at{core::Time::epoch()};
  std::optional<std::string> trigger_key{};
  std::optional<std::string> trigger_payload{};
};

struct AutomationPromptRunResult {
  std::string text;
};

using AutomationPromptRunner =
    std::function<async::Awaitable<core::Result<AutomationPromptRunResult>>(AutomationPromptRunRequest)>;

[[nodiscard]] AutomationPromptRunRequest make_cron_prompt_run_request(const CronDueJob& due);
[[nodiscard]] AutomationPromptRunRequest make_triggered_prompt_run_request(const TriggeredExecutionJob& execution);

[[nodiscard]] CronJobHandler make_cron_prompt_handler(AutomationPromptRunner runner);
[[nodiscard]] TriggeredJobHandler make_triggered_prompt_handler(AutomationPromptRunner runner);

}  // namespace orangutan::automation

// src/oran-bootstrap/automation_prompt_runner.cpp - bootstrap-owned automation prompt bridge.

#include <oran/bootstrap/automation_prompt_runner.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/config.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/permission/approval.hpp>

namespace orangutan::bootstrap {
namespace {

using ::orangutan::core::Error;
using ::orangutan::core::Result;

[[nodiscard]] Error option_error(std::string message) {
  return Error::invalid_argument(std::move(message));
}

[[nodiscard]] Result<void> validate_options(const AutomationAgentPromptRunnerOptions& options) {
  if (options.assembly == nullptr) {
    return std::unexpected(option_error("automation prompt runner bridge requires a runtime assembly"));
  }
  if (options.config == nullptr) {
    return std::unexpected(option_error("automation prompt runner bridge requires a config"));
  }
  if (options.provider == nullptr) {
    return std::unexpected(option_error("automation prompt runner bridge requires a provider system"));
  }
  if (!options.executor) {
    return std::unexpected(option_error("automation prompt runner bridge requires an executor"));
  }
  if (options.route.primary.profile.empty()) {
    return std::unexpected(option_error("automation prompt runner bridge route primary profile must not be empty"));
  }
  if (options.route.primary.model.empty()) {
    return std::unexpected(option_error("automation prompt runner bridge route primary model must not be empty"));
  }
  if (options.scope_key.empty()) {
    return std::unexpected(option_error("automation prompt runner bridge scope key must not be empty"));
  }
  if (options.identity.empty()) {
    return std::unexpected(option_error("automation prompt runner bridge identity must not be empty"));
  }
  if (options.origin.empty()) {
    return std::unexpected(option_error("automation prompt runner bridge origin must not be empty"));
  }
  if (options.trace_context_json.empty()) {
    return std::unexpected(option_error("automation prompt runner bridge trace context JSON must not be empty"));
  }
  return {};
}

[[nodiscard]] Result<void> validate_request(const automation::AutomationPromptRunRequest& request) {
  if (request.job_key.empty()) {
    return std::unexpected(
        option_error("automation prompt request job key must not be empty").with("field", "job_key"));
  }
  if (request.agent_key.empty()) {
    return std::unexpected(
        option_error("automation prompt request agent key must not be empty").with("field", "agent_key"));
  }
  if (request.prompt.empty()) {
    return std::unexpected(option_error("automation prompt request prompt must not be empty").with("field", "prompt"));
  }
  if (request.job_type == automation::AutomationPromptJobType::triggered && !request.trigger_key.has_value()) {
    return std::unexpected(
        option_error("automation triggered prompt request requires a trigger key").with("field", "trigger_key"));
  }
  return {};
}

[[nodiscard]] bool has_agent_config(const config::Config& cfg, std::string_view agent_key) noexcept {
  const auto agents = cfg.agents();
  return std::ranges::find(agents, agent_key, &config::AgentConfig::name) != agents.end();
}

[[nodiscard]] core::TurnId session_id_for(std::string_view scope_key,
                                          const automation::AutomationPromptRunRequest& request) {
  const auto key =
      std::format("{}\n{}\n{}\n{}", core::enum_name(request.job_type), scope_key, request.job_key, request.agent_key);
  const auto hash = permission::ApprovalAuthority::input_hash(key);
  auto session_id = core::TurnId{};
  std::copy_n(hash.begin(), session_id.size(), session_id.begin());
  if (core::is_zero_turn_id(session_id)) {
    session_id.back() = std::byte{0x01};
  }
  return session_id;
}

[[nodiscard]] AgentPromptRunnerOptions runner_options_for(const AutomationAgentPromptRunnerOptions& base,
                                                          const automation::AutomationPromptRunRequest& request) {
  const auto apply_agent_overlay = has_agent_config(*base.config, request.agent_key);
  return AgentPromptRunnerOptions{
      .executor = base.executor,
      .assembly = base.assembly,
      .config = base.config,
      .provider = base.provider,
      .route = base.route,
      .mode = base.mode,
      .agent_config_name = apply_agent_overlay ? request.agent_key : std::string{},
      .permission_agent_name = apply_agent_overlay ? request.agent_key : std::string{},
      .scope_key = base.scope_key,
      .agent_key = request.agent_key,
      .identity = base.identity,
      .origin = base.origin,
      .system_preamble = base.system_preamble,
      .skills_catalog = base.skills_catalog,
      .skills_directory = base.skills_directory,
      .longterm_recall = base.longterm_recall,
      .longterm_hybrid_search = base.longterm_hybrid_search,
      .memory_framing = base.memory_framing,
      .per_agent_overlay = base.per_agent_overlay,
      .trace_context_json = base.trace_context_json,
      .tool_choice = base.tool_choice,
      .max_tokens = base.max_tokens,
      .thinking_budget = base.thinking_budget,
      .retry = base.retry,
      .stream = false,
      .session_id = session_id_for(base.scope_key, request),
      .approval_answers = base.approval_answers,
      .quiet = true,
      .stream_out = nullptr,
      .bind_operator_prompt_sink = base.bind_operator_prompt_sink,
      .registry = base.registry,
      .scheduler = base.scheduler,
  };
}

}  // namespace

core::Result<automation::AutomationPromptRunner>
make_automation_agent_prompt_runner(AutomationAgentPromptRunnerOptions options) {
  if (auto valid = validate_options(options); !valid) {
    return std::unexpected(std::move(valid).error());
  }

  return automation::AutomationPromptRunner{
      [options = std::move(options)](automation::AutomationPromptRunRequest request)
          -> async::Awaitable<core::Result<automation::AutomationPromptRunResult>> {
        if (auto valid = validate_request(request); !valid) {
          co_return std::unexpected(std::move(valid).error());
        }

        auto runner = AgentPromptRunner::create(runner_options_for(options, request));
        if (!runner) {
          co_return std::unexpected(std::move(runner).error());
        }

        auto prompt_result = co_await (*runner)->run_prompt(
            cli::PromptRunRequest{.prompt = std::move(request.prompt), .mode = cli::CliMode::single_shot});
        if (!prompt_result) {
          co_return std::unexpected(std::move(prompt_result).error());
        }

        co_return automation::AutomationPromptRunResult{.text = std::move(prompt_result->text)};
      }};
}

}  // namespace orangutan::bootstrap

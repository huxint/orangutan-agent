// src/oran-bootstrap/channel_prompt_runner.cpp - bootstrap-owned channel prompt bridge.

#include <oran/bootstrap/channel_prompt_runner.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/config.hpp>
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

[[nodiscard]] Result<void> validate_options(const ChannelAgentPromptRunnerOptions& options) {
  if (options.assembly == nullptr) {
    return std::unexpected(option_error("channel prompt runner bridge requires a runtime assembly"));
  }
  if (options.config == nullptr) {
    return std::unexpected(option_error("channel prompt runner bridge requires a config"));
  }
  if (options.provider == nullptr) {
    return std::unexpected(option_error("channel prompt runner bridge requires a provider system"));
  }
  if (!options.executor) {
    return std::unexpected(option_error("channel prompt runner bridge requires an executor"));
  }
  if (options.route.primary.profile.empty()) {
    return std::unexpected(option_error("channel prompt runner bridge route primary profile must not be empty"));
  }
  if (options.route.primary.model.empty()) {
    return std::unexpected(option_error("channel prompt runner bridge route primary model must not be empty"));
  }
  if (options.agent_key.empty()) {
    return std::unexpected(option_error("channel prompt runner bridge agent key must not be empty"));
  }
  if (options.scope_key.empty()) {
    return std::unexpected(option_error("channel prompt runner bridge scope key must not be empty"));
  }
  if (options.identity.empty()) {
    return std::unexpected(option_error("channel prompt runner bridge identity must not be empty"));
  }
  if (options.origin.empty()) {
    return std::unexpected(option_error("channel prompt runner bridge origin must not be empty"));
  }
  if (options.trace_context_json.empty()) {
    return std::unexpected(option_error("channel prompt runner bridge trace context JSON must not be empty"));
  }
  return {};
}

[[nodiscard]] Result<void> validate_request(const channel::ChannelPromptRunRequest& request) {
  if (request.channel_id.empty()) {
    return std::unexpected(
        option_error("channel prompt request channel id must not be empty").with("field", "channel_id"));
  }
  if (request.conversation_id.empty()) {
    return std::unexpected(
        option_error("channel prompt request conversation id must not be empty").with("field", "conversation_id"));
  }
  if (request.prompt.empty()) {
    return std::unexpected(option_error("channel prompt request prompt must not be empty").with("field", "prompt"));
  }
  return {};
}

[[nodiscard]] bool has_agent_config(const config::Config& cfg, std::string_view agent_key) noexcept {
  const auto agents = cfg.agents();
  return std::ranges::find(agents, agent_key, &config::AgentConfig::name) != agents.end();
}

[[nodiscard]] core::TurnId session_id_for(const ChannelAgentPromptRunnerOptions& options,
                                          const channel::ChannelPromptRunRequest& request) {
  const auto key = std::format("channel\n{}\n{}\n{}\n{}",
                               options.scope_key,
                               request.channel_id,
                               request.conversation_id,
                               options.agent_key);
  const auto hash = permission::ApprovalAuthority::input_hash(key);
  auto session_id = core::TurnId{};
  std::copy_n(hash.begin(), session_id.size(), session_id.begin());
  if (core::is_zero_turn_id(session_id)) {
    session_id.back() = std::byte{0x01};
  }
  return session_id;
}

[[nodiscard]] AgentPromptRunnerOptions runner_options_for(const ChannelAgentPromptRunnerOptions& base,
                                                          const channel::ChannelPromptRunRequest& request) {
  const auto apply_agent_overlay = has_agent_config(*base.config, base.agent_key);
  return AgentPromptRunnerOptions{
      .executor = base.executor,
      .assembly = base.assembly,
      .config = base.config,
      .provider = base.provider,
      .route = base.route,
      .mode = base.mode,
      .agent_config_name = apply_agent_overlay ? base.agent_key : std::string{},
      .permission_agent_name = apply_agent_overlay ? base.agent_key : std::string{},
      .scope_key = base.scope_key,
      .agent_key = base.agent_key,
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
      .session_id = session_id_for(base, request),
      .approval_answers = base.approval_answers,
      .quiet = true,
      .stream_out = nullptr,
      .bind_operator_prompt_sink = base.bind_operator_prompt_sink,
  };
}

}  // namespace

core::Result<channel::ChannelPromptRunner> make_channel_agent_prompt_runner(ChannelAgentPromptRunnerOptions options) {
  if (auto valid = validate_options(options); !valid) {
    return std::unexpected(std::move(valid).error());
  }

  return channel::ChannelPromptRunner{[options = std::move(options)](channel::ChannelPromptRunRequest request)
                                          -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
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

    co_return channel::ChannelPromptRunResult{.text = std::move(prompt_result->text)};
  }};
}

}  // namespace orangutan::bootstrap

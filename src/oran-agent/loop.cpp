// src/oran-agent/loop.cpp — first `agent::Loop` provider drive.

#include <oran/agent/loop.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <oran/core/error.hpp>
#include <oran/core/role.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::agent {
namespace {

constexpr auto kDefaultActiveTools = std::array<std::string_view, 6>{
    "file.read",
    "file.write",
    "file.edit",
    "file.search",
    "directory.list",
    "tool.search",
};

[[nodiscard]] bool is_promoted_tool(std::span<const std::string> promoted_tools, std::string_view name) noexcept {
  return std::ranges::contains(promoted_tools, name);
}

[[nodiscard]] bool is_default_active_tool(std::string_view name) noexcept {
  return std::ranges::contains(kDefaultActiveTools, name);
}

[[nodiscard]] bool is_explicit_active_tool(const config::PromptActiveToolsConfig& active_tools,
                                           std::string_view name) noexcept {
  return !active_tools.use_defaults && std::ranges::contains(active_tools.tool_names, name);
}

/// Mirror the prompt builder's active-tool selection just far enough to
/// populate `provider::Request::tools`. The prompt bytes are still the source
/// of truth for what the model reads; this native list lets adapters that have
/// first-class tool fields send the same active set without parsing section 2.
///
/// The builder owns validation. We intentionally call it before this helper in
/// `run_turn`, so an explicit unknown tool name fails at the prompt boundary
/// instead of being silently omitted here.
[[nodiscard]] std::vector<core::ToolDef> request_tools_for(std::span<const core::ToolDef> catalog,
                                                           const config::PromptActiveToolsConfig& active_tools,
                                                           std::span<const std::string> promoted_tools) {
  std::vector<core::ToolDef> selected;
  selected.reserve(catalog.size());
  for (const auto& def : catalog) {
    if ((active_tools.use_defaults && is_default_active_tool(def.name)) ||
        is_explicit_active_tool(active_tools, def.name) || is_promoted_tool(promoted_tools, def.name)) {
      selected.push_back(def);
    }
  }
  std::ranges::sort(selected, {}, &core::ToolDef::name);
  return selected;
}

[[nodiscard]] std::optional<std::string> join_prompt_prefix(const prompt::RenderedPrompt& rendered) {
  if (rendered.sections.empty()) {
    return std::nullopt;
  }

  std::string output;
  output.reserve(rendered.prefix_bytes + rendered.sections.size());
  for (const auto& section : rendered.sections) {
    if (section.id == "conversation_tail") {
      break;
    }
    if (!output.empty()) {
      output.push_back('\n');
    }
    output.append(section.content);
  }
  return output;
}

void append_text_block(std::string& output, std::string_view text) {
  if (!output.empty() && !text.empty()) {
    output.push_back('\n');
  }
  output.append(text);
}

[[nodiscard]] std::string assemble_terminal_text(std::span<const core::Content> blocks) {
  std::string text;
  for (const auto& block : blocks) {
    if (auto* content = std::get_if<core::TextContent>(&block); content != nullptr) {
      append_text_block(text, content->text);
    }
  }
  return text;
}

[[nodiscard]] core::Error unsupported_response(std::string reason) {
  return core::Error::internal("agent loop: response requires a later loop slice").with("reason", std::move(reason));
}

[[nodiscard]] core::Error iteration_cap_error(std::uint32_t max_iterations) {
  return core::Error::internal("agent loop: iteration cap reached")
      .with("reason", "iteration_cap")
      .with("max_iterations", std::to_string(max_iterations));
}

[[nodiscard]] core::Error with_cancellation_phase(core::Error error, std::string_view phase) {
  if (error.kind() != core::ErrorKind::cancelled) {
    return error;
  }
  return std::move(error).with("reason", "parent_cancelled").with("cancellation_phase", std::string{phase});
}

[[nodiscard]] std::vector<core::ToolUseContent> tool_uses_in(std::span<const core::Content> blocks) {
  std::vector<core::ToolUseContent> uses;
  for (const auto& block : blocks) {
    if (auto* tool = std::get_if<core::ToolUseContent>(&block); tool != nullptr) {
      uses.push_back(*tool);
    }
  }
  return uses;
}

void add_usage(provider::Usage& total, const provider::Usage& next) {
  total.input_tokens += next.input_tokens;
  total.output_tokens += next.output_tokens;
  total.cache_creation_tokens += next.cache_creation_tokens;
  total.cache_read_tokens += next.cache_read_tokens;
  if (next.cost_estimate.has_value()) {
    total.cost_estimate = total.cost_estimate.value_or(0.0) + *next.cost_estimate;
  }
}

[[nodiscard]] std::string render_tool_error(const core::Error& error) {
  std::string output;
  output.reserve(error.message().size() + 64);
  output.append("tool error: ");
  output.append(error.message());
  for (const auto& [key, value] : error.context()) {
    output.append("\n");
    output.append(key);
    output.append(": ");
    output.append(value);
  }
  return output;
}

[[nodiscard]] bool model_visible_tool_error(core::ErrorKind kind) noexcept {
  switch (kind) {
    case core::ErrorKind::cancelled:
    case core::ErrorKind::storage:
    case core::ErrorKind::internal:
      return false;
    default:
      return true;
  }
}

[[nodiscard]] core::Result<core::ToolResultContent> tool_result_from(std::string tool_use_id,
                                                                     core::Result<tool::Output> output) {
  if (output.has_value()) {
    return core::ToolResultContent{
        .tool_use_id = std::move(tool_use_id),
        .output = std::move(output->text),
        .is_error = output->is_error,
    };
  }
  if (!model_visible_tool_error(output.error().kind())) {
    return std::unexpected(std::move(output).error());
  }
  return core::ToolResultContent{
      .tool_use_id = std::move(tool_use_id),
      .output = render_tool_error(output.error()),
      .is_error = true,
  };
}

}  // namespace

class Loop::Impl {
public:
  Impl(provider::System& provider, provider::Route route, LoopOptions options)
      : provider_{provider}, route_{std::move(route)}, options_{std::move(options)}, builder_{options_.prompt_options} {
  }

  [[nodiscard]] async::Awaitable<core::Result<RunTurnResult>> run_turn(RunTurnInputs inputs,
                                                                       provider::EventSink* sink) {
    std::vector<core::Message> transcript{inputs.conversation_tail.begin(), inputs.conversation_tail.end()};
    auto total_usage = provider::Usage{};
    const auto native_tools = request_tools_for(inputs.tool_catalog, inputs.active_tools, inputs.promoted_tools);
    const auto thinking_budget =
        inputs.thinking_budget.has_value() ? inputs.thinking_budget : route_.primary.thinking_budget;

    for (std::uint32_t iteration = 1; iteration <= options_.max_iterations; ++iteration) {
      auto rendered = co_await builder_.build(prompt::BuilderInputs{
          .system_preamble = inputs.system_preamble,
          .tool_catalog = inputs.tool_catalog,
          .active_tools = inputs.active_tools,
          .promoted_tools = inputs.promoted_tools,
          .skills_catalog = inputs.skills_catalog,
          .memory_framing = inputs.memory_framing,
          .per_agent_overlay = inputs.per_agent_overlay,
          .conversation_tail = transcript,
      });
      if (!rendered) {
        co_return std::unexpected(std::move(rendered).error());
      }

      auto cache =
          provider::make_prompt_cache_hints(*rendered, route_.primary.cache.value_or(provider::PromptCacheOptions{}));
      if (!cache) {
        co_return std::unexpected(std::move(cache).error());
      }

      auto request = provider::Request{
          .messages = transcript,
          .system_prompt = join_prompt_prefix(*rendered),
          .tools = native_tools,
          .tool_choice = inputs.tool_choice,
          .max_tokens = inputs.max_tokens,
          .thinking_budget = thinking_budget,
          .stream = inputs.stream,
          .cache = *cache,
          .retry = inputs.retry,
      };

      auto response = co_await provider_.send(std::move(request), route_, sink);
      if (!response) {
        co_return std::unexpected(with_cancellation_phase(std::move(response).error(), "provider"));
      }

      add_usage(total_usage, response->usage);

      const auto tool_uses = tool_uses_in(response->blocks);
      if (response->stop_reason == core::StopReason::tool_use || !tool_uses.empty()) {
        if (inputs.tools == nullptr || inputs.dispatch_context == nullptr) {
          co_return std::unexpected(unsupported_response("tool_use response"));
        }
        if (tool_uses.empty()) {
          co_return std::unexpected(unsupported_response("tool_use stop reason without tool blocks"));
        }

        transcript.push_back(core::Message{
            .role = core::Role::assistant,
            .blocks = response->blocks,
            .created_at = std::nullopt,
        });

        std::vector<core::Content> tool_results;
        tool_results.reserve(tool_uses.size());
        for (const auto& use : tool_uses) {
          auto output = co_await inputs.tools->dispatch(use.name, use.input_json, *inputs.dispatch_context);
          auto tool_result = tool_result_from(use.id, std::move(output));
          if (!tool_result) {
            co_return std::unexpected(with_cancellation_phase(std::move(tool_result).error(), "tools"));
          }
          tool_results.emplace_back(std::move(*tool_result));
        }
        transcript.push_back(core::Message{
            .role = core::Role::tool,
            .blocks = std::move(tool_results),
            .created_at = std::nullopt,
        });
        continue;
      }

      if (response->stop_reason != core::StopReason::end_turn &&
          response->stop_reason != core::StopReason::stop_sequence &&
          response->stop_reason != core::StopReason::max_tokens) {
        co_return std::unexpected(unsupported_response("non-terminal stop reason"));
      }

      auto text = assemble_terminal_text(response->blocks);
      transcript.push_back(core::Message{
          .role = core::Role::assistant,
          .blocks = response->blocks,
          .created_at = std::nullopt,
      });
      co_return RunTurnResult{
          .text = std::move(text),
          .assistant_blocks = std::move(response->blocks),
          .stop_reason = response->stop_reason,
          .usage = total_usage,
          .model_used = std::move(response->model_used),
          .rendered_prompt = std::move(*rendered),
          .cache_hints = std::move(*cache),
          .iterations = iteration,
          .transcript = std::move(transcript),
      };
    }

    co_return std::unexpected(iteration_cap_error(options_.max_iterations));
  }

  [[nodiscard]] const provider::Route& route() const noexcept {
    return route_;
  }

  [[nodiscard]] const LoopOptions& options() const noexcept {
    return options_;
  }

private:
  provider::System& provider_;
  provider::Route route_;
  LoopOptions options_;
  prompt::Builder builder_;
};

Loop::Loop(provider::System& provider, provider::Route route, LoopOptions options)
    : impl_{std::make_unique<Impl>(provider, std::move(route), std::move(options))} {}

Loop::~Loop() = default;

Loop::Loop(Loop&&) noexcept = default;

Loop& Loop::operator=(Loop&&) noexcept = default;

async::Awaitable<core::Result<RunTurnResult>> Loop::run_turn(RunTurnInputs inputs, provider::EventSink* sink) {
  return impl_->run_turn(inputs, sink);
}

const provider::Route& Loop::route() const noexcept {
  return impl_->route();
}

const LoopOptions& Loop::options() const noexcept {
  return impl_->options();
}

}  // namespace orangutan::agent

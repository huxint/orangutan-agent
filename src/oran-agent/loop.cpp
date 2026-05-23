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

[[nodiscard]] bool contains_tool_use(std::span<const core::Content> blocks) noexcept {
  return std::ranges::any_of(blocks, [](const core::Content& block) {
    return std::holds_alternative<core::ToolUseContent>(block);
  });
}

[[nodiscard]] core::Error unsupported_response(std::string reason) {
  return core::Error::internal("agent loop: response requires a later loop slice").with("reason", std::move(reason));
}

}  // namespace

class Loop::Impl {
public:
  Impl(provider::System& provider, provider::Route route, LoopOptions options)
      : provider_{provider}, route_{std::move(route)}, options_{std::move(options)}, builder_{options_.prompt_options} {
  }

  [[nodiscard]] async::Awaitable<core::Result<RunTurnResult>> run_turn(RunTurnInputs inputs,
                                                                       provider::EventSink* sink) {
    auto rendered = co_await builder_.build(prompt::BuilderInputs{
        .system_preamble = inputs.system_preamble,
        .tool_catalog = inputs.tool_catalog,
        .active_tools = inputs.active_tools,
        .promoted_tools = inputs.promoted_tools,
        .skills_catalog = inputs.skills_catalog,
        .memory_framing = inputs.memory_framing,
        .per_agent_overlay = inputs.per_agent_overlay,
        .conversation_tail = inputs.conversation_tail,
    });
    if (!rendered) {
      co_return std::unexpected(std::move(rendered).error());
    }

    auto cache =
        provider::make_prompt_cache_hints(*rendered, route_.primary.cache.value_or(provider::PromptCacheOptions{}));
    if (!cache) {
      co_return std::unexpected(std::move(cache).error());
    }

    const auto thinking_budget =
        inputs.thinking_budget.has_value() ? inputs.thinking_budget : route_.primary.thinking_budget;

    auto request = provider::Request{
        .messages = std::vector<core::Message>{inputs.conversation_tail.begin(), inputs.conversation_tail.end()},
        .system_prompt = join_prompt_prefix(*rendered),
        .tools = request_tools_for(inputs.tool_catalog, inputs.active_tools, inputs.promoted_tools),
        .tool_choice = inputs.tool_choice,
        .max_tokens = inputs.max_tokens,
        .thinking_budget = thinking_budget,
        .stream = inputs.stream,
        .cache = *cache,
        .retry = inputs.retry,
    };

    auto response = co_await provider_.send(std::move(request), route_, sink);
    if (!response) {
      co_return std::unexpected(std::move(response).error());
    }

    // This first loop slice is allowed to finish a text-style assistant turn;
    // it is not allowed to quietly consume a tool-use turn. Returning a loud
    // error here keeps scenario #2+#3 honest until the next slice appends
    // tool_result blocks and re-enters the provider.
    if (response->stop_reason == core::StopReason::tool_use || contains_tool_use(response->blocks)) {
      co_return std::unexpected(unsupported_response("tool_use response"));
    }
    if (response->stop_reason != core::StopReason::end_turn &&
        response->stop_reason != core::StopReason::stop_sequence &&
        response->stop_reason != core::StopReason::max_tokens) {
      co_return std::unexpected(unsupported_response("non-terminal stop reason"));
    }

    auto text = assemble_terminal_text(response->blocks);
    co_return RunTurnResult{
        .text = std::move(text),
        .assistant_blocks = std::move(response->blocks),
        .stop_reason = response->stop_reason,
        .usage = response->usage,
        .model_used = std::move(response->model_used),
        .rendered_prompt = std::move(*rendered),
        .cache_hints = std::move(*cache),
        .iterations = 1,
    };
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

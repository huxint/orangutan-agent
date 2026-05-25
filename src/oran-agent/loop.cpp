// src/oran-agent/loop.cpp — first `agent::Loop` provider drive.

#include <oran/agent/loop.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/cancellation_state.hpp>
#include <asio/this_coro.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/role.hpp>
#include <oran/core/time.hpp>
#include <oran/storage/trace_repository.hpp>
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

[[nodiscard]] std::int64_t now_epoch_ns() noexcept {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] core::Result<std::int64_t> checked_i64(std::uint64_t value, std::string field) {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(
        core::Error::invalid_argument("trace counter exceeds storage range").with("field", std::move(field)));
  }
  return static_cast<std::int64_t>(value);
}

[[nodiscard]] core::Result<std::int64_t> checked_size_i64(std::size_t value, std::string field) {
  if (value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(
        core::Error::invalid_argument("trace byte count exceeds storage range").with("field", std::move(field)));
  }
  return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::uint64_t section_hash(const prompt::RenderedPrompt& rendered, std::string_view id) noexcept {
  const auto it = std::ranges::find(rendered.sections, id, &prompt::CacheSection::id);
  return it == rendered.sections.end() ? 0 : it->content_hash;
}

[[nodiscard]] std::optional<core::TurnId> dispatch_parent_turn_id(const RunTurnInputs& inputs) {
  if (!inputs.trace.enabled) {
    return std::nullopt;
  }
  return inputs.turn_id;
}

[[nodiscard]] bool trace_writer_configured(const RunTurnInputs& inputs) noexcept {
  return inputs.trace.enabled && inputs.trace.repository != nullptr;
}

void write_u64_be(core::TurnId& id, std::size_t offset, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8; ++index) {
    const auto shift = static_cast<unsigned>((7 - index) * 8);
    id[offset + index] = static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

[[nodiscard]] core::Result<core::TurnId> generate_turn_id() {
  try {
    static std::atomic<std::uint64_t> sequence{0};
    std::random_device random;
    const auto random_hi = static_cast<std::uint64_t>(random()) << 32U;
    const auto random_lo = static_cast<std::uint64_t>(random());
    const auto entropy = random_hi | random_lo;
    const auto counter = sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
    const auto timestamp = static_cast<std::uint64_t>(now_epoch_ns());

    core::TurnId id{};
    write_u64_be(id, 0, entropy ^ std::rotl(timestamp, 17));
    write_u64_be(id, 8, counter ^ std::rotl(entropy, 31) ^ timestamp);

    id[6] = (id[6] & std::byte{0x0f}) | std::byte{0x40};
    id[8] = (id[8] & std::byte{0x3f}) | std::byte{0x80};
    if (core::is_zero_turn_id(id)) {
      id[15] = std::byte{0x01};
    }
    return id;
  } catch (const std::exception& error) {
    return std::unexpected(
        core::Error::internal("agent loop: failed to generate turn id").with("reason", error.what()));
  } catch (...) {
    return std::unexpected(core::Error::internal("agent loop: failed to generate turn id").with("reason", "unknown"));
  }
}

[[nodiscard]] core::Result<storage::AppendTraceTurnRequest>
make_trace_request(const RunTurnInputs& inputs,
                   const provider::Route& route,
                   const prompt::RenderedPrompt& rendered,
                   const provider::Usage& usage,
                   std::uint32_t iterations,
                   std::int64_t started_at_ns,
                   std::string route_model,
                   core::StopReason stop_reason,
                   std::optional<std::string> cancellation_phase = std::nullopt) {
  if (inputs.trace.repository == nullptr) {
    return std::unexpected(core::Error::invalid_argument("trace repository is not configured"));
  }
  if (!inputs.trace.enabled) {
    return std::unexpected(core::Error::invalid_argument("trace writer is disabled"));
  }
  if (!inputs.turn_id.has_value()) {
    return std::unexpected(core::Error::invalid_argument("trace writer requires a turn id"));
  }

  auto prefix_bytes = checked_size_i64(rendered.prefix_bytes, "prompt_prefix_bytes");
  if (!prefix_bytes) {
    return std::unexpected(prefix_bytes.error());
  }
  auto input_tokens = checked_i64(usage.input_tokens, "input_tokens");
  if (!input_tokens) {
    return std::unexpected(input_tokens.error());
  }
  auto output_tokens = checked_i64(usage.output_tokens, "output_tokens");
  if (!output_tokens) {
    return std::unexpected(output_tokens.error());
  }
  auto cache_creation_tokens = checked_i64(usage.cache_creation_tokens, "cache_creation_tokens");
  if (!cache_creation_tokens) {
    return std::unexpected(cache_creation_tokens.error());
  }
  auto cache_read_tokens = checked_i64(usage.cache_read_tokens, "cache_read_tokens");
  if (!cache_read_tokens) {
    return std::unexpected(cache_read_tokens.error());
  }

  return storage::AppendTraceTurnRequest{
      .turn_id = *inputs.turn_id,
      .parent_turn_id = inputs.trace.parent_turn_id,
      .session_id = inputs.trace.session_id,
      .agent_key = std::string{inputs.trace.agent_key},
      .origin = std::string{inputs.trace.origin},
      .route_profile = route.primary.profile,
      .route_model = std::move(route_model),
      .started_at_ns = started_at_ns,
      .finished_at_ns = now_epoch_ns(),
      .stop_reason = std::string{core::enum_name(stop_reason)},
      .iteration_count = static_cast<std::int64_t>(iterations),
      .prompt_prefix_hash = rendered.prefix_hash,
      .prompt_prefix_bytes = *prefix_bytes,
      .active_catalog_hash = section_hash(rendered, "tool_catalog"),
      .deferred_catalog_hash = section_hash(rendered, "deferred_tools"),
      .cache_creation_tokens = *cache_creation_tokens,
      .cache_read_tokens = *cache_read_tokens,
      .input_tokens = *input_tokens,
      .output_tokens = *output_tokens,
      .cost_estimate_usd = usage.cost_estimate.value_or(0.0),
      .cancellation_phase = std::move(cancellation_phase),
      .context_json = std::string{inputs.trace.context_json},
  };
}

[[nodiscard]] async::Awaitable<core::Result<void>>
write_trace_turn(const RunTurnInputs& inputs,
                 const provider::Route& route,
                 const prompt::RenderedPrompt& rendered,
                 const provider::Usage& usage,
                 std::uint32_t iterations,
                 std::int64_t started_at_ns,
                 std::string route_model,
                 core::StopReason stop_reason,
                 std::optional<std::string> cancellation_phase = std::nullopt) {
  if (!trace_writer_configured(inputs)) {
    co_return core::Result<void>{};
  }
  auto request = make_trace_request(inputs,
                                    route,
                                    rendered,
                                    usage,
                                    iterations,
                                    started_at_ns,
                                    std::move(route_model),
                                    stop_reason,
                                    std::move(cancellation_phase));
  if (!request) {
    co_return std::unexpected(std::move(request).error());
  }
  auto appended = co_await inputs.trace.repository->append_turn(std::move(*request));
  if (!appended) {
    co_return std::unexpected(std::move(appended).error());
  }
  co_return core::Result<void>{};
}

[[nodiscard]] async::Awaitable<core::Result<void>> write_error_trace_turn(const RunTurnInputs& inputs,
                                                                          const provider::Route& route,
                                                                          const prompt::RenderedPrompt& rendered,
                                                                          const provider::Usage& usage,
                                                                          std::uint32_t iterations,
                                                                          std::int64_t started_at_ns,
                                                                          std::string route_model) {
  if (!trace_writer_configured(inputs)) {
    co_return core::Result<void>{};
  }
  co_return co_await write_trace_turn(inputs,
                                      route,
                                      rendered,
                                      usage,
                                      iterations,
                                      started_at_ns,
                                      std::move(route_model),
                                      core::StopReason::error);
}

class ScopedDispatchContext {
public:
  ScopedDispatchContext(tool::DispatchContext& context, std::optional<core::TurnId> parent_turn_id) noexcept
      : context_{&context}, previous_parent_turn_id_{context.parent_turn_id}, previous_now_{context.now} {
    context.parent_turn_id = std::move(parent_turn_id);
    context.now = core::time::now_utc();
  }

  ~ScopedDispatchContext() {
    context_->parent_turn_id = std::move(previous_parent_turn_id_);
    context_->now = previous_now_;
  }

  ScopedDispatchContext(const ScopedDispatchContext&) = delete;
  ScopedDispatchContext& operator=(const ScopedDispatchContext&) = delete;

private:
  tool::DispatchContext* context_;
  std::optional<core::TurnId> previous_parent_turn_id_;
  core::Time previous_now_;
};

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
        .data_json = std::move(output->data_json),
        .is_error = output->is_error,
    };
  }
  if (!model_visible_tool_error(output.error().kind())) {
    return std::unexpected(std::move(output).error());
  }
  return core::ToolResultContent{
      .tool_use_id = std::move(tool_use_id),
      .output = render_tool_error(output.error()),
      .data_json = std::nullopt,
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
    if (trace_writer_configured(inputs) && !inputs.turn_id.has_value()) {
      auto generated = generate_turn_id();
      if (!generated) {
        co_return std::unexpected(std::move(generated).error());
      }
      inputs.turn_id = *generated;
    }

    std::vector<core::Message> transcript{inputs.conversation_tail.begin(), inputs.conversation_tail.end()};
    auto total_usage = provider::Usage{};
    const auto started_at_ns = now_epoch_ns();
    const auto native_tools = request_tools_for(inputs.tool_catalog, inputs.active_tools, inputs.promoted_tools);
    const auto thinking_budget =
        inputs.thinking_budget.has_value() ? inputs.thinking_budget : route_.primary.thinking_budget;
    std::optional<prompt::RenderedPrompt> last_rendered;
    std::string last_route_model = route_.primary.model;

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
        auto error = with_cancellation_phase(std::move(response).error(), "provider");
        if (error.kind() == core::ErrorKind::cancelled && trace_writer_configured(inputs)) {
          co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
          auto traced = co_await write_trace_turn(inputs,
                                                  route_,
                                                  *rendered,
                                                  total_usage,
                                                  iteration,
                                                  started_at_ns,
                                                  route_.primary.model,
                                                  core::StopReason::cancelled,
                                                  std::string{"provider"});
          if (!traced) {
            co_return std::unexpected(std::move(traced).error());
          }
        } else if (error.kind() != core::ErrorKind::cancelled) {
          auto traced = co_await write_error_trace_turn(inputs,
                                                        route_,
                                                        *rendered,
                                                        total_usage,
                                                        iteration,
                                                        started_at_ns,
                                                        route_.primary.model);
          if (!traced) {
            co_return std::unexpected(std::move(traced).error());
          }
        }
        co_return std::unexpected(std::move(error));
      }

      add_usage(total_usage, response->usage);
      last_rendered = *rendered;
      last_route_model = response->model_used.value_or(route_.primary.model);

      const auto tool_uses = tool_uses_in(response->blocks);
      if (response->stop_reason == core::StopReason::tool_use || !tool_uses.empty()) {
        if (inputs.tools == nullptr || inputs.dispatch_context == nullptr) {
          auto error = unsupported_response("tool_use response");
          const auto route_model = response->model_used.value_or(route_.primary.model);
          auto traced = co_await write_error_trace_turn(inputs,
                                                        route_,
                                                        *rendered,
                                                        total_usage,
                                                        iteration,
                                                        started_at_ns,
                                                        route_model);
          if (!traced) {
            co_return std::unexpected(std::move(traced).error());
          }
          co_return std::unexpected(std::move(error));
        }
        if (tool_uses.empty()) {
          auto error = unsupported_response("tool_use stop reason without tool blocks");
          const auto route_model = response->model_used.value_or(route_.primary.model);
          auto traced = co_await write_error_trace_turn(inputs,
                                                        route_,
                                                        *rendered,
                                                        total_usage,
                                                        iteration,
                                                        started_at_ns,
                                                        route_model);
          if (!traced) {
            co_return std::unexpected(std::move(traced).error());
          }
          co_return std::unexpected(std::move(error));
        }

        transcript.push_back(core::Message{
            .role = core::Role::assistant,
            .blocks = response->blocks,
            .created_at = std::nullopt,
        });

        std::vector<core::Content> tool_results;
        tool_results.reserve(tool_uses.size());
        for (const auto& use : tool_uses) {
          core::Result<tool::Output> output;
          {
            ScopedDispatchContext dispatch_context{*inputs.dispatch_context, dispatch_parent_turn_id(inputs)};
            output = co_await inputs.tools->dispatch(use.name, use.input_json, *inputs.dispatch_context);
          }
          auto tool_result = tool_result_from(use.id, std::move(output));
          if (!tool_result) {
            auto error = with_cancellation_phase(std::move(tool_result).error(), "tools");
            if (error.kind() == core::ErrorKind::cancelled && trace_writer_configured(inputs)) {
              co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
              const auto route_model = response->model_used.value_or(route_.primary.model);
              auto traced = co_await write_trace_turn(inputs,
                                                      route_,
                                                      *rendered,
                                                      total_usage,
                                                      iteration,
                                                      started_at_ns,
                                                      route_model,
                                                      core::StopReason::cancelled,
                                                      std::string{"tools"});
              if (!traced) {
                co_return std::unexpected(std::move(traced).error());
              }
            } else if (error.kind() != core::ErrorKind::cancelled) {
              const auto route_model = response->model_used.value_or(route_.primary.model);
              auto traced = co_await write_error_trace_turn(inputs,
                                                            route_,
                                                            *rendered,
                                                            total_usage,
                                                            iteration,
                                                            started_at_ns,
                                                            route_model);
              if (!traced) {
                co_return std::unexpected(std::move(traced).error());
              }
            }
            co_return std::unexpected(std::move(error));
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
        auto error = unsupported_response("non-terminal stop reason");
        const auto route_model = response->model_used.value_or(route_.primary.model);
        auto traced = co_await write_error_trace_turn(inputs,
                                                      route_,
                                                      *rendered,
                                                      total_usage,
                                                      iteration,
                                                      started_at_ns,
                                                      route_model);
        if (!traced) {
          co_return std::unexpected(std::move(traced).error());
        }
        co_return std::unexpected(std::move(error));
      }

      auto text = assemble_terminal_text(response->blocks);
      transcript.push_back(core::Message{
          .role = core::Role::assistant,
          .blocks = response->blocks,
          .created_at = std::nullopt,
      });
      const auto model_used = response->model_used.value_or(route_.primary.model);
      if (auto traced = co_await write_trace_turn(inputs,
                                                  route_,
                                                  *rendered,
                                                  total_usage,
                                                  iteration,
                                                  started_at_ns,
                                                  model_used,
                                                  response->stop_reason);
          !traced) {
        co_return std::unexpected(std::move(traced).error());
      }
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

    if (trace_writer_configured(inputs) && last_rendered.has_value()) {
      auto traced = co_await write_error_trace_turn(inputs,
                                                    route_,
                                                    *last_rendered,
                                                    total_usage,
                                                    options_.max_iterations,
                                                    started_at_ns,
                                                    last_route_model);
      if (!traced) {
        co_return std::unexpected(std::move(traced).error());
      }
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

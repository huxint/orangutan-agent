// include/oran/agent/loop.hpp — first fake-provider-backed agent loop.
//
// This header opens the real `oran-agent` runtime surface without pulling the
// tool scheduler, memory runtime, storage audit rows, or blocking hooks into
// the first loop increments. The intent is deliberate: spec 0017 says the loop must be
// proven against `provider::FakeProvider` before any vendor adapter ships. This
// class is that seam. It builds the cached prompt, maps it into a
// `provider::Request`, sends provider iterations, and accepts terminal
// text-style responses. When the caller supplies the existing registry dispatch
// boundary, it can also run the first sequential tool-use iteration path and
// re-enter the provider.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/config/config.hpp>
#include <oran/core/content.hpp>
#include <oran/core/message.hpp>
#include <oran/core/result.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/prompt/builder.hpp>
#include <oran/provider/cache.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::tool {
class Registry;
struct DispatchContext;
}  // namespace orangutan::tool

namespace orangutan::storage {
class TraceRepository;
}  // namespace orangutan::storage

namespace orangutan::agent {

struct LoopOptions {
  /// Spec 0017 makes the iteration cap a runtime invariant. The current loop
  /// consumes repeated provider turns sequentially; the future scheduler keeps
  /// the same cap at the agent boundary.
  std::uint32_t max_iterations{16};
  prompt::BuilderOptions prompt_options{};

  friend bool operator==(const LoopOptions&, const LoopOptions&) = default;
};

struct TraceContext {
  /// Operator trace switch after the caller maps `config::TraceConfig` into
  /// turn inputs. `false` preserves trace-disabled bytes: no `trace_turns` row
  /// is written and direct tool audit rows keep `parent_turn_id = NULL`.
  bool enabled{true};
  /// Optional storage writer for spec-0018 per-turn trace rows. When null, the
  /// loop preserves the pre-trace behavior and only `turn_id` audit stamping can
  /// occur if `enabled` is true. When non-null, `RunTurnInputs::turn_id`,
  /// `session_id`, `agent_key`, and `origin` must also be set.
  storage::TraceRepository* repository{nullptr};
  core::TurnId session_id{};
  std::optional<core::TurnId> parent_turn_id{};
  std::string_view agent_key{};
  std::string_view origin{};
  std::string_view context_json{"{}"};
};

struct RunTurnInputs {
  /// Stable section (1). The caller owns the actual preamble text for now;
  /// later slices move the template into `oran-agent` once its wording is
  /// stable enough to become a prompt contract.
  std::string_view system_preamble{};
  /// Catalog snapshot from `tool::Registry::catalog()`. `Loop` forwards it to
  /// `prompt::Builder` and also mirrors the active subset, sorted by tool name,
  /// into `provider::Request::tools` so future adapters do not have to parse
  /// prompt text to discover native tool definitions.
  std::span<const core::ToolDef> tool_catalog{};
  config::PromptActiveToolsConfig active_tools{};
  /// Sorted promotion snapshot from `SessionState::promotion_snapshot(now)`.
  std::span<const std::string> promoted_tools{};
  std::string_view skills_catalog{};
  std::string_view memory_framing{};
  std::string_view per_agent_overlay{};
  /// Section (7), already including the current user turn. The loop copies
  /// these messages into the provider request; the span only needs to remain
  /// valid until the coroutine is awaited to completion.
  std::span<const core::Message> conversation_tail{};
  std::optional<std::string> tool_choice{};
  std::optional<std::uint32_t> max_tokens{};
  std::optional<std::uint32_t> thinking_budget{};
  provider::RetryPolicy retry{};
  bool stream{true};
  /// Optional trace/audit correlation id for this turn. When set and
  /// `trace.enabled` is true, the loop threads it into every direct tool
  /// dispatch as `DispatchContext::parent_turn_id`; when unset, dispatch audit
  /// rows keep `parent_turn_id = NULL` for trace-disabled and pre-trace callers.
  std::optional<core::TurnId> turn_id{};
  /// Optional per-turn trace writer context. This first writer slice records
  /// redacted `trace_turns` rows when a caller supplies both `trace.repository`
  /// and `turn_id`; ID generation, operator config, and CLI inspection remain
  /// downstream.
  TraceContext trace{};
  /// Optional direct-dispatch bridge for spec 0017 scenarios #2/#3. Both
  /// pointers must be non-null to execute tool_use blocks; otherwise the loop
  /// still fails loudly on tool_use so callers do not accidentally run a
  /// partial ReAct loop without permission/audit infrastructure.
  tool::Registry* tools{nullptr};
  tool::DispatchContext* dispatch_context{nullptr};
};

struct RunTurnResult {
  /// Text assembled from every `TextContent` block in the terminal assistant
  /// response. `assistant_blocks` preserves the typed response for future
  /// storage / UI consumers that should not re-parse this fallback string.
  std::string text;
  std::vector<core::Content> assistant_blocks;
  core::StopReason stop_reason{core::StopReason::end_turn};
  provider::Usage usage{};
  std::optional<std::string> model_used{};
  prompt::RenderedPrompt rendered_prompt{};
  std::optional<provider::PromptCacheHints> cache_hints{};
  std::uint32_t iterations{0};
  /// Complete transcript tail after the turn, including the terminal assistant
  /// response. Tool-loop callers can persist this value as the turn's
  /// working-memory delta until the real session repository owner lands.
  std::vector<core::Message> transcript;
};

class Loop {
public:
  Loop(provider::System& provider, provider::Route route, LoopOptions options = {});
  ~Loop();

  Loop(const Loop&) = delete;
  Loop& operator=(const Loop&) = delete;
  Loop(Loop&&) noexcept;
  Loop& operator=(Loop&&) noexcept;

  /// Run one user turn through the current MVP loop. This is intentionally
  /// narrower than the final ReAct loop: it sends requests sequentially,
  /// accepts terminal text-style stop reasons, and only dispatches tool_use
  /// blocks when `RunTurnInputs::tools` and `dispatch_context` are supplied.
  /// Parallel scheduling, turn audit rows, blocking approval rendering, and
  /// provider retry/fallback remain later slices. Parent cancellation during
  /// the provider await or direct tool dispatch is surfaced as
  /// `ErrorKind::cancelled` with `reason=parent_cancelled` plus
  /// `cancellation_phase=provider|tools`; when a trace context is configured,
  /// the same phase is persisted before the cancelled result is returned.
  [[nodiscard]] async::Awaitable<core::Result<RunTurnResult>> run_turn(RunTurnInputs inputs,
                                                                       provider::EventSink* sink = nullptr);

  [[nodiscard]] const provider::Route& route() const noexcept;
  [[nodiscard]] const LoopOptions& options() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::agent

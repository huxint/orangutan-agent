// include/oran/agent/loop.hpp — first fake-provider-backed agent loop.
//
// This header opens the real `oran-agent` runtime surface without pulling the
// tool scheduler, memory runtime, storage audit rows, or blocking hooks into
// the first slice. The intent is deliberate: spec 0017 says the loop must be
// proven against `provider::FakeProvider` before any vendor adapter ships. This
// class is that seam. It builds the cached prompt, maps it into a
// `provider::Request`, sends exactly one provider iteration, and accepts only
// terminal text-style responses. Tool-use iteration is a later slice.

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
#include <oran/prompt/builder.hpp>
#include <oran/provider/cache.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::agent {

struct LoopOptions {
  /// Spec 0017 makes the iteration cap a runtime invariant. The first loop
  /// slice performs one provider iteration only, but storing the cap now keeps
  /// the public construction shape stable when the tool loop starts consuming
  /// repeated provider turns.
  std::uint32_t max_iterations{16};
  prompt::BuilderOptions prompt_options{};

  friend bool operator==(const LoopOptions&, const LoopOptions&) = default;
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
  /// narrower than the final ReAct loop: it sends one request and accepts
  /// terminal text-style stop reasons. `StopReason::tool_use` returns an
  /// explicit `internal` error so callers and tests cannot mistake the first
  /// slice for a complete tool-dispatch implementation.
  [[nodiscard]] async::Awaitable<core::Result<RunTurnResult>> run_turn(RunTurnInputs inputs,
                                                                       provider::EventSink* sink = nullptr);

  [[nodiscard]] const provider::Route& route() const noexcept;
  [[nodiscard]] const LoopOptions& options() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::agent

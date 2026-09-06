
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/agent/prompt.hpp>
#include <oran/agent/scheduler.hpp>
#include <oran/core/result.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/permission/rule_set.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::config {
class Config;
}  // namespace orangutan::config

namespace orangutan::bootstrap {

class RuntimeAssembly;

enum class LongtermRecallQueryStrategy : std::uint8_t {
  prompt_text,
  last_user_message,
};

struct LongtermRecallOptions {
  bool enabled{false};
  std::size_t limit{5};
  /// `prompt_text` preserves the current one-query behavior. `last_user_message`
  /// lets follow-up prompts recall from the previous user text while still
  /// reading long-term memory once before the loop.
  LongtermRecallQueryStrategy query_strategy{LongtermRecallQueryStrategy::prompt_text};
  /// Optional `memory::longterm::RecordKind` spellings to include. Empty means
  /// the runtime searches every non-shadow kind.
  std::vector<std::string> kinds{};

  friend bool operator==(const LongtermRecallOptions&, const LongtermRecallOptions&) = default;
};

struct LongtermHybridSearchOptions {
  bool enabled{false};
  std::size_t lexical_limit{10};
  std::size_t vector_limit{10};
  std::size_t result_limit{10};
  double lexical_weight{1.0};
  double vector_weight{1.0};
  std::string embedding_model{"oran-local-text-v1"};
  std::size_t embedding_dimensions{64};

  friend bool operator==(const LongtermHybridSearchOptions&, const LongtermHybridSearchOptions&) = default;
};

struct AgentSessionOptions {
  asio::any_io_executor executor{};
  asio::any_io_executor blocking_executor{};
  RuntimeAssembly* assembly{nullptr};
  const config::Config* config{nullptr};
  provider::System* provider{nullptr};
  provider::Route route{};
  permission::Mode mode{permission::Mode::default_};
  /// Optional `agents.<name>` entry for per-agent prompt/runtime config such
  /// as `prompt_overlay` and `skills_enabled`. Empty falls back to
  /// `permission_agent_name` so current selected-agent callers keep one selector.
  std::string agent_config_name{};
  std::string permission_agent_name{};
  std::string scope_key{"default"};
  std::string agent_key{"default"};
  std::string identity{"owner"};
  std::string origin{"owner"};
  /// Optional section-1 override. Empty uses the loop-owned default system
  /// preamble from `oran-agent`.
  std::string system_preamble{};
  /// Optional pre-rendered section-4 skill catalog. Empty means no activated
  /// skills are listed; skill bodies remain outside this runner option.
  std::string skills_catalog{};
  /// Optional skills directory to snapshot before the first prompt. Missing
  /// directory means an empty catalog; when `skills_catalog` is non-empty this
  /// path is ignored so tests and embedders can provide exact section bytes.
  std::string skills_directory{};
  /// Optional prompt-boundary long-term memory recall. Disabled by default so
  /// embedders/config can opt in explicitly after choosing query policy.
  LongtermRecallOptions longterm_recall{};
  /// Optional hybrid long-term search policy. When enabled, prompt-boundary
  /// recall and `MemoryRecall` combine lexical search with the assembly-owned
  /// vector backend; memory writes/deletes maintain the vector index.
  LongtermHybridSearchOptions longterm_hybrid_search{};
  std::string memory_framing{};
  /// Optional exact section-6 overlay bytes. Empty lets the selected
  /// `agents.<name>.prompt_overlay` value fill the section.
  std::string per_agent_overlay{};
  std::string trace_context_json{"{}"};
  std::optional<std::string> tool_choice{std::string{"auto"}};
  std::optional<std::uint32_t> max_tokens{4096};
  std::optional<std::uint32_t> thinking_budget{};
  provider::RetryPolicy retry{};
  bool stream{true};
  core::TurnId session_id{};
  provider::EventSink* event_sink{nullptr};
  /// Optional externally-owned tool registry + scheduler. Default (both null)
  /// keeps the runner self-owned: it builds its own builtin `tool::Registry`
  /// and an `agent::ToolScheduler` over it (the CLI / channel / desktop path).
  /// When BOTH are set the runner borrows them instead, so a long-lived owner
  /// (e.g. `orangutan --serve`) can share one scheduler across many short-lived
  /// per-job runners and drive its idle-lock reaping centrally. They must be
  /// set together (`create` rejects exactly one), must reference the same
  /// registry the scheduler dispatches through, and must outlive every runner
  /// built from these options. The scheduler's per-path lock table is
  /// single-strand by contract, so `executor` must be the one strand the owner
  /// also reaps on. Borrowed for the runner's lifetime.
  tool::Registry* registry{nullptr};
  agent::ToolScheduler* scheduler{nullptr};
};

/// Map `runtime.tool_scheduler.*` config into `agent::ToolSchedulerOptions`,
/// rejecting a non-positive `max_parallel_tools`. Shared between
/// `AgentSession::create` (the self-owned scheduler) and the `--serve`
/// owner that builds one shared scheduler to reap; keeping it here is the one
/// place the config→options mapping lives.
[[nodiscard]] core::Result<agent::ToolSchedulerOptions> scheduler_options_from(const config::Config& config);

/// Owns one agent session over borrowed runtime services. Calls are serialized
/// by the owning service; the assembly and provider outlive the session.
class AgentSession {
public:
  /// Passkey for the public constructor — `AgentSession` is built only
  /// through `create`, which constructs the tag internally so callers cannot
  /// bypass the factory's validation.
  class PrivateTag {
    PrivateTag() = default;
    friend class AgentSession;
  };

  [[nodiscard]] static core::Result<std::unique_ptr<AgentSession>> create(AgentSessionOptions options);

  ~AgentSession();

  AgentSession(const AgentSession&) = delete;
  AgentSession& operator=(const AgentSession&) = delete;
  AgentSession(AgentSession&&) = delete;
  AgentSession& operator=(AgentSession&&) = delete;

  [[nodiscard]] async::Awaitable<core::Result<agent::PromptResult>> run_prompt(agent::PromptRequest request);

  [[nodiscard]] std::size_t prompts_processed() const noexcept;
  /// Count of `ToolSearch` results the runner fed back into the per-session
  /// `agent::SessionState` after each turn. The counter increments once per
  /// observed `ToolSearch` tool_result, including ones that returned no
  /// deferred matches.
  [[nodiscard]] std::size_t tool_search_observations_recorded() const noexcept;
  /// Count of prompt memory-framing renders performed at the runner boundary.
  /// A multi-iteration ReAct turn increments this once, before `agent::Loop`.
  [[nodiscard]] std::size_t memory_framing_renders() const noexcept;
  /// Count of stable section-1 renders performed by the runner. A multi-iteration
  /// turn increments this once, before `agent::Loop`.
  [[nodiscard]] std::size_t system_preamble_renders() const noexcept;
  /// Count of skill-catalog section renders performed by the runner boundary.
  /// A multi-iteration turn increments this once, before `agent::Loop`.
  [[nodiscard]] std::size_t skill_catalog_renders() const noexcept;
  /// Count of directory snapshots the runner loaded through `oran-skill`.
  /// This stays at zero when callers provide `skills_catalog` directly.
  [[nodiscard]] std::size_t skill_catalog_loads() const noexcept;
  [[nodiscard]] const provider::Route& route() const noexcept;

  class Impl;
  /// Construct from an already-validated `Impl`. Public-but-tagged so
  /// `std::make_unique` can invoke it from `create`. Constructing a
  /// `PrivateTag` outside `AgentSession` is impossible (the default
  /// constructor is private + only `AgentSession` is a friend).
  AgentSession(std::unique_ptr<Impl> impl, PrivateTag) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::bootstrap

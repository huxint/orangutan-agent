
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

struct LongtermRecallOptions {
  bool enabled{true};
  std::size_t limit{20};
  std::vector<std::string> kinds{};
};

struct AgentSessionOptions {
  asio::any_io_executor executor{};
  asio::any_io_executor blocking_executor{};
  RuntimeAssembly* assembly{nullptr};
  const config::Config* config{nullptr};
  provider::System* provider{nullptr};
  provider::Route route{};
  permission::Mode mode{permission::Mode::default_};
  std::string agent_config_name{};
  std::string scope_key{"default"};
  std::string agent_key{"default"};
  std::string identity{"owner"};
  std::string origin{"owner"};
  std::string system_preamble{};
  /// Absent: use config.memory.longterm.recall when a backend is available and
  /// exact memory_framing is empty. A supplied value is an explicit override.
  std::optional<LongtermRecallOptions> longterm_recall{};
  std::string memory_framing{};
  std::string per_agent_overlay{};
  std::string trace_context_json{"{}"};
  std::optional<std::string> tool_choice{std::string{"auto"}};
  std::optional<std::uint32_t> max_tokens{4096};
  std::optional<std::uint32_t> thinking_budget{};
  provider::RetryPolicy retry{};
  bool stream{true};
  core::TurnId session_id{};
  /// Correlates a child turn with the parent's tool call in the trace repository.
  std::optional<core::TurnId> parent_turn_id{};
  /// Child sessions borrow the parent's immutable rules until run_prompt joins.
  /// A session with an inherited policy cannot delegate further.
  std::optional<permission::PolicyView> parent_policy{};
  /// Maximum child sessions admitted per prompt; zero disables delegation.
  std::size_t max_child_runs{4};
  provider::EventSink* event_sink{nullptr};
  /// Inject both to share a scheduler. Services outlive this session and all
  /// tools; executor must be the strand that owns the shared scheduler.
  tool::Registry* registry{nullptr};
  agent::ToolScheduler* scheduler{nullptr};
};

[[nodiscard]] core::Result<agent::ToolSchedulerOptions> scheduler_options_from(const config::Config& config);

/// Owns one agent session over borrowed runtime services. Calls are serialized
/// by the owning service; the assembly and provider outlive the session.
class AgentSession {
public:
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

  [[nodiscard]] const provider::Route& route() const noexcept;

  class Impl;
  AgentSession(std::unique_ptr<Impl> impl, PrivateTag) noexcept;

private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::bootstrap

// include/oran/bootstrap/automation_prompt_runner.hpp - bootstrap-owned automation prompt bridge.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/automation.hpp>
#include <oran/bootstrap/prompt_runner.hpp>
#include <oran/core/result.hpp>
#include <oran/permission/rule_set.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::config {
class Config;
}  // namespace orangutan::config

namespace orangutan::bootstrap {

class RuntimeAssembly;

struct AutomationAgentPromptRunnerOptions {
  asio::any_io_executor executor{};
  RuntimeAssembly* assembly{nullptr};
  const config::Config* config{nullptr};
  provider::System* provider{nullptr};
  provider::Route route{};
  permission::Mode mode{permission::Mode::default_};
  std::string scope_key{"automation"};
  std::string identity{"automation"};
  std::string origin{"automation"};
  std::string system_preamble{};
  std::string skills_catalog{};
  std::string skills_directory{};
  LongtermRecallOptions longterm_recall{};
  LongtermHybridSearchOptions longterm_hybrid_search{};
  std::string memory_framing{};
  std::string per_agent_overlay{};
  std::string trace_context_json{"{}"};
  std::optional<std::string> tool_choice{std::string{"auto"}};
  std::optional<std::uint32_t> max_tokens{};
  std::optional<std::uint32_t> thinking_budget{};
  provider::RetryPolicy retry{};
  std::vector<std::string> approval_answers{};
  bool bind_operator_prompt_sink{false};
  /// Optional externally-owned tool registry + scheduler, forwarded verbatim to
  /// every per-job `AgentPromptRunner`. Default (both null) lets each job own
  /// its scheduler. The `--serve` owner sets both to one shared, strand-driven
  /// scheduler so a single idle-lock reaping tick bounds the lock table across
  /// all jobs (see `AgentPromptRunnerOptions::scheduler`). Borrowed for the
  /// bridge's lifetime; must outlive every job execution.
  tool::Registry* registry{nullptr};
  agent::ToolScheduler* scheduler{nullptr};
};

/// Build a bootstrap-owned bridge from automation prompt requests to
/// `AgentPromptRunner`.
///
/// The returned callback keeps automation caller-owned: it does not open
/// `automation.db`, start a scheduler loop, or own a background service.
/// Instead it builds one `AgentPromptRunner` per automation job execution using
/// the supplied provider backend and runtime assembly. The durable automation
/// job identity derives a stable per-job session id so later executions can
/// reuse persisted transcript state when session memory is enabled.
[[nodiscard]] core::Result<automation::AutomationPromptRunner>
make_automation_agent_prompt_runner(AutomationAgentPromptRunnerOptions options);

}  // namespace orangutan::bootstrap

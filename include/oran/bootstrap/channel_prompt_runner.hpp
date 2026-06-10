// include/oran/bootstrap/channel_prompt_runner.hpp - bootstrap-owned channel prompt bridge.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/bootstrap/prompt_runner.hpp>
#include <oran/channel/dispatch.hpp>
#include <oran/core/result.hpp>
#include <oran/permission/rule_set.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::config {
class Config;
}  // namespace orangutan::config

namespace orangutan::bootstrap {

class RuntimeAssembly;

struct ChannelAgentPromptRunnerOptions {
  asio::any_io_executor executor{};
  RuntimeAssembly* assembly{nullptr};
  const config::Config* config{nullptr};
  provider::System* provider{nullptr};
  provider::Route route{};
  permission::Mode mode{permission::Mode::default_};
  /// Agent that answers messages handed to this bridge. Config-driven
  /// per-channel agent routing arrives with the bootstrap registration slice;
  /// until then one bridge serves one configured agent.
  std::string agent_key{"default"};
  std::string scope_key{"channel"};
  std::string identity{"channel"};
  std::string origin{"channel"};
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
};

/// Build a bootstrap-owned bridge from channel prompt requests to
/// `AgentPromptRunner`.
///
/// The returned runner keeps channel ingress caller-owned: it does not
/// register adapters, start receive loops, or own a `ChannelManager`. It
/// builds one `AgentPromptRunner` per dispatched message using the supplied
/// provider backend and runtime assembly. The channel/conversation identity
/// derives a stable per-conversation session id so follow-up messages reuse
/// persisted transcript state when session memory is enabled.
[[nodiscard]] core::Result<channel::ChannelPromptRunner>
make_channel_agent_prompt_runner(ChannelAgentPromptRunnerOptions options);

}  // namespace orangutan::bootstrap

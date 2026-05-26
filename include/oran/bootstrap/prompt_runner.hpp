// include/oran/bootstrap/prompt_runner.hpp - bootstrap-owned CLI prompt runner.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/cli/cli.hpp>
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

struct AgentPromptRunnerOptions {
  asio::any_io_executor executor{};
  RuntimeAssembly* assembly{nullptr};
  const config::Config* config{nullptr};
  provider::System* provider{nullptr};
  provider::Route route{};
  permission::Mode mode{permission::Mode::default_};
  std::string permission_agent_name{};
  std::string scope_key{"default"};
  std::string agent_key{"default"};
  std::string identity{"terminal"};
  std::string origin{"cli"};
  std::string system_preamble{};
  std::string skills_catalog{};
  std::string memory_framing{};
  std::string per_agent_overlay{};
  std::string trace_context_json{"{}"};
  std::optional<std::string> tool_choice{std::string{"auto"}};
  std::optional<std::uint32_t> max_tokens{};
  std::optional<std::uint32_t> thinking_budget{};
  provider::RetryPolicy retry{};
  bool stream{true};
  core::TurnId session_id{};
  std::vector<std::string> approval_answers{};
  bool quiet{false};
};

/// Adapter-neutral bridge from `cli::run_async` into `agent::Loop`.
///
/// The runner borrows `RuntimeAssembly`, `config::Config`, and the caller's
/// provider backend for its lifetime. It owns the builtin tool registry,
/// materialized permission rules, provider execution wrapper, CLI operator
/// approval sink binding, and transcript tail for successive prompts. Real
/// provider adapter construction remains a bootstrap concern outside this
/// class; tests and `bootstrap::run` can supply any `provider::System`.
class AgentPromptRunner final : public cli::PromptRunner {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<AgentPromptRunner>> create(AgentPromptRunnerOptions options);

  ~AgentPromptRunner() override;

  AgentPromptRunner(const AgentPromptRunner&) = delete;
  AgentPromptRunner& operator=(const AgentPromptRunner&) = delete;
  AgentPromptRunner(AgentPromptRunner&&) = delete;
  AgentPromptRunner& operator=(AgentPromptRunner&&) = delete;

  [[nodiscard]] async::Awaitable<core::Result<cli::PromptRunResult>> run_prompt(cli::PromptRunRequest request) override;

  [[nodiscard]] std::size_t prompts_processed() const noexcept;
  [[nodiscard]] std::size_t approval_prompts_rendered() const noexcept;
  [[nodiscard]] const provider::Route& route() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;

  explicit AgentPromptRunner(std::unique_ptr<Impl> impl) noexcept;
};

}  // namespace orangutan::bootstrap

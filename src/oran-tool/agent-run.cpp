#include <oran/tool/builtins.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <oran/core/str.hpp>

#include "_impl/parse_input.hpp"

namespace orangutan::tool {
namespace {

constexpr std::size_t MAX_AGENT_PROMPT_BYTES = 16384;

async::Awaitable<core::Result<Output>>
run_agent(std::span<const std::string> agent_names, std::string_view input, DispatchContext& context) {
  auto parsed = detail::parse_input_object(input, AGENT_RUN_NAME);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed).error());
  }
  constexpr auto fields = std::array<std::string_view, 2>{"agent", "prompt"};
  for (const auto& [field, value] : parsed->items()) {
    if (!std::ranges::contains(fields, field)) {
      co_return std::unexpected(core::Error::invalid_argument("AgentRun: unknown input field").with("field", field));
    }
  }
  auto agent = detail::require_string_field(*parsed, AGENT_RUN_NAME, "agent");
  if (!agent) {
    co_return std::unexpected(std::move(agent).error());
  }
  if (!std::ranges::contains(agent_names, *agent)) {
    co_return std::unexpected(core::Error::invalid_argument("AgentRun: agent is not configured").with("agent", *agent));
  }
  auto prompt = detail::require_string_field(*parsed, AGENT_RUN_NAME, "prompt");
  if (!prompt) {
    co_return std::unexpected(std::move(prompt).error());
  }
  if (prompt->empty() || prompt->size() > MAX_AGENT_PROMPT_BYTES || !core::str::is_valid_utf8(*prompt)) {
    co_return std::unexpected(
        core::Error::invalid_argument("AgentRun: prompt must contain 1–16384 bytes of UTF-8 text"));
  }
  if (!context.agent_run) {
    co_return std::unexpected(core::Error::permission_denied("AgentRun: delegation is disabled for this session")
                                  .with("reason", "delegation_disabled"));
  }
  co_return co_await context.agent_run(AgentRunRequest{.agent = std::move(*agent), .prompt = std::move(*prompt)},
                                       context);
}

}  // namespace

core::Result<void> register_agent_run(Registry& registry, std::span<const std::string> agent_names) {
  if (agent_names.empty() || std::ranges::any_of(agent_names, [](const auto& name) {
        return name.empty() || !core::str::is_valid_utf8(name);
      })) {
    return std::unexpected(core::Error::invalid_argument("AgentRun requires configured agent names"));
  }
  auto names = std::vector<std::string>{agent_names.begin(), agent_names.end()};
  const auto schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {{"agent", {{"type", "string"}, {"enum", names}}},
        {"prompt", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAX_AGENT_PROMPT_BYTES}}}}},
      {"required", {"agent", "prompt"}},
      {"additionalProperties", false},
  };
  return registry.add(
      core::ToolDef{
          .name = std::string{AGENT_RUN_NAME},
          .description = "Run a configured child agent and return its completed answer. Supply a self-contained task; "
                         "each call starts a fresh conversation. The child uses this workspace and memory scope "
                         "under both agents' permissions.",
          .input_schema_json = schema.dump(),
          .required_capabilities = {core::Capability::spawn_agent},
          .deferred = false,
          .category = "agent",
      },
      [names = std::move(names)](std::string_view input, DispatchContext& context) {
        return run_agent(names, input, context);
      });
}

}  // namespace orangutan::tool

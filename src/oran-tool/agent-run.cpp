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
constexpr auto kAgentRunFields = std::to_array<std::string_view>({"agent", "prompt"});

core::Result<AgentRunRequest> parse_agent_run(std::span<const std::string> agent_names, std::string_view input) {
  auto parsed = detail::parse_input_object(input, AGENT_RUN_NAME, kAgentRunFields);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }
  auto agent = detail::require_string_field(*parsed, AGENT_RUN_NAME, "agent");
  if (!agent) {
    return std::unexpected(std::move(agent).error());
  }
  if (!std::ranges::contains(agent_names, *agent)) {
    return std::unexpected(core::Error::invalid_argument("AgentRun: agent is not configured").with("agent", *agent));
  }
  auto prompt = detail::require_string_field(*parsed, AGENT_RUN_NAME, "prompt");
  if (!prompt) {
    return std::unexpected(std::move(prompt).error());
  }
  if (prompt->empty() || prompt->size() > MAX_AGENT_PROMPT_BYTES || !core::str::is_valid_utf8(*prompt)) {
    return std::unexpected(core::Error::invalid_argument("AgentRun: prompt must contain 1–16384 bytes of UTF-8 text"));
  }
  return AgentRunRequest{.agent = std::move(*agent), .prompt = std::move(*prompt)};
}

async::Awaitable<core::Result<Output>> run_agent(AgentRunRequest request, DispatchContext& context) {
  if (!context.agent_run) {
    co_return std::unexpected(core::Error::permission_denied("AgentRun: delegation is disabled for this session")
                                  .with("reason", "delegation_disabled"));
  }
  co_return co_await context.agent_run(std::move(request), context);
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
  return registry.add_prepared(
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
      [names = std::move(names)](std::string_view input) -> core::Result<PreparedCall> {
        auto request = parse_agent_run(names, input);
        if (!request) {
          return std::unexpected(std::move(request).error());
        }
        return PreparedCall{.path = std::nullopt,
                            .execute = [request = std::move(*request)](DispatchContext& context) mutable {
                              return run_agent(std::move(request), context);
                            }};
      });
}

}  // namespace orangutan::tool

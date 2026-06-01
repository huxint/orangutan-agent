// src/oran-tool/skill_invoke.cpp - `skill.invoke` built-in.

#include <oran/tool/builtins.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/tool/output.hpp>
#include <oran/tool/registry.hpp>

#include "_impl/parse_input.hpp"

namespace orangutan::tool {
namespace {

using json = nlohmann::json;

constexpr std::string_view kSkillInvokeSchema =
    R"({"type":"object","properties":{"name":{"type":"string"},"inputs":{}},"required":["name"],"additionalProperties":false})";

struct ParsedInvoke {
  std::string name;
  std::string inputs_json{"{}"};
};

[[nodiscard]] core::Result<ParsedInvoke> parse_invoke(std::string_view input_json) {
  auto parsed = detail::parse_input_object(input_json, kSkillInvokeName);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto name = detail::require_string_field(*parsed, kSkillInvokeName, "name");
  if (!name) {
    return std::unexpected(std::move(name).error());
  }
  if (name->empty()) {
    return std::unexpected(core::Error::invalid_argument("skill.invoke: `name` must be non-empty"));
  }

  auto inputs_json = std::string{"{}"};
  const auto inputs = parsed->find("inputs");
  if (inputs != parsed->end()) {
    inputs_json = inputs->dump();
  }

  return ParsedInvoke{
      .name = std::move(*name),
      .inputs_json = std::move(inputs_json),
  };
}

[[nodiscard]] async::Awaitable<core::Result<Output>> skill_invoke_handler(std::string_view input_json,
                                                                          DispatchContext& ctx) {
  auto parsed = parse_invoke(input_json);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed).error());
  }
  if (!ctx.skill_invoke) {
    co_return std::unexpected(core::Error::invalid_argument("skill.invoke: runtime service is not available")
                                  .with("reason", "skill_runtime_unavailable")
                                  .with("skill", parsed->name));
  }

  co_return co_await ctx.skill_invoke(parsed->name, parsed->inputs_json, ctx);
}

}  // namespace

core::Result<void> register_skill_invoke(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kSkillInvokeName},
      .description = "Invoke a loaded markdown skill by name using optional structured inputs. The skill body is "
                     "returned as a tool result for the next model iteration; skill files are loaded from the "
                     "runtime's current skill snapshot.",
      .input_schema_json = std::string{kSkillInvokeSchema},
      .required_capabilities = {core::Capability::invoke_skill},
      .deferred = false,
      .category = "skill",
  };

  return registry.add(std::move(def), &skill_invoke_handler);
}

}  // namespace orangutan::tool

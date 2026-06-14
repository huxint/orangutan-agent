// src/oran-tool/skill_deactivate.cpp - `SkillDeactivate` built-in.

#include <oran/tool/builtins.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/tool/output.hpp>
#include <oran/tool/registry.hpp>

#include "_impl/parse_input.hpp"

namespace orangutan::tool {
namespace {

constexpr std::string_view kSkillDeactivateSchema =
    R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"],"additionalProperties":false})";

[[nodiscard]] core::Result<std::string> parse_deactivate(std::string_view input_json) {
  auto parsed = detail::parse_input_object(input_json, kSkillDeactivateName);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto name = detail::require_string_field(*parsed, kSkillDeactivateName, "name");
  if (!name) {
    return std::unexpected(std::move(name).error());
  }
  if (name->empty()) {
    return std::unexpected(core::Error::invalid_argument("SkillDeactivate: `name` must be non-empty"));
  }
  return std::move(*name);
}

[[nodiscard]] async::Awaitable<core::Result<Output>> skill_deactivate_handler(std::string_view input_json,
                                                                              DispatchContext& ctx) {
  auto name = parse_deactivate(input_json);
  if (!name) {
    co_return std::unexpected(std::move(name).error());
  }
  if (!ctx.skill_deactivate) {
    co_return std::unexpected(core::Error::invalid_argument("SkillDeactivate: runtime service is not available")
                                  .with("reason", "skill_runtime_unavailable")
                                  .with("skill", *name));
  }

  co_return co_await ctx.skill_deactivate(*name, ctx);
}

}  // namespace

core::Result<void> register_skill_deactivate(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kSkillDeactivateName},
      .description = "Deactivate a loaded markdown skill by name so it stops appearing as active in the prompt's "
                     "skill catalog from the next turn onward. Use this when a skill's guidance is no longer "
                     "relevant; the skill stays available to invoke again later.",
      .input_schema_json = std::string{kSkillDeactivateSchema},
      .required_capabilities = {core::Capability::deactivate_skill},
      .deferred = false,
      .category = "skill",
  };

  return registry.add(std::move(def), &skill_deactivate_handler);
}

}  // namespace orangutan::tool

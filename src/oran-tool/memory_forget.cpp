// src/oran-tool/memory_forget.cpp - `MemoryForget` built-in.

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

constexpr std::string_view kMemoryForgetSchema =
    R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"],"additionalProperties":false})";

[[nodiscard]] core::Result<std::string> require_non_empty_id(const json& parsed) {
  auto id = detail::require_string_field(parsed, kMemoryForgetName, "id");
  if (!id) {
    return std::unexpected(std::move(id).error());
  }
  if (id->empty()) {
    return std::unexpected(core::Error::invalid_argument("MemoryForget: `id` must be non-empty").with("field", "id"));
  }
  return std::move(*id);
}

[[nodiscard]] core::Result<MemoryForgetRequest> parse_forget(std::string_view input_json) {
  auto parsed = detail::parse_input_object(input_json, kMemoryForgetName);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto id = require_non_empty_id(*parsed);
  if (!id) {
    return std::unexpected(std::move(id).error());
  }

  return MemoryForgetRequest{
      .id = std::move(*id),
  };
}

[[nodiscard]] async::Awaitable<core::Result<Output>> memory_forget_handler(std::string_view input_json,
                                                                           DispatchContext& ctx) {
  auto parsed = parse_forget(input_json);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed).error());
  }
  if (!ctx.memory_forget) {
    co_return std::unexpected(core::Error::invalid_argument("MemoryForget: runtime service is not available")
                                  .with("reason", "memory_runtime_unavailable")
                                  .with("id", parsed->id));
  }

  co_return co_await ctx.memory_forget(std::move(*parsed), ctx);
}

}  // namespace

core::Result<void> register_memory_forget(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kMemoryForgetName},
      .description = "Remove one long-term memory record in the current agent scope. Deletes are idempotent; the "
                     "result returns confirmation text plus structured removed-key metadata.",
      .input_schema_json = std::string{kMemoryForgetSchema},
      .required_capabilities = {core::Capability::write_memory},
      .deferred = true,
      .category = "memory",
  };

  return registry.add(std::move(def), &memory_forget_handler);
}

}  // namespace orangutan::tool

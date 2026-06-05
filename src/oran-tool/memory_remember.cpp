// src/oran-tool/memory_remember.cpp - `memory.remember` built-in.

#include <oran/tool/builtins.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

constexpr double kDefaultMemoryImportance = 0.5;

constexpr std::string_view kMemoryRememberSchema =
    R"({"type":"object","properties":{"id":{"type":"string"},"kind":{"type":"string","enum":["user","feedback","project","reference","team"]},"title":{"type":"string"},"body":{"type":"string"},"importance":{"type":"number","minimum":0,"maximum":1},"tags":{"type":"array","items":{"type":"string"},"uniqueItems":true},"linked_record_ids":{"type":"array","items":{"type":"string"},"uniqueItems":true},"shadow":{"type":"boolean"}},"required":["id","kind","title","body"],"additionalProperties":false})";

[[nodiscard]] core::Result<std::string> require_non_empty_string(const json& parsed, std::string_view field) {
  auto value = detail::require_string_field(parsed, kMemoryRememberName, field);
  if (!value) {
    return std::unexpected(std::move(value).error());
  }
  if (value->empty()) {
    return std::unexpected(core::Error::invalid_argument("memory.remember: string field must be non-empty")
                               .with("field", std::string{field}));
  }
  return std::move(*value);
}

[[nodiscard]] core::Result<double> parse_importance(const json& parsed) {
  if (!parsed.contains("importance")) {
    return kDefaultMemoryImportance;
  }
  const auto& raw = parsed["importance"];
  if (!raw.is_number()) {
    return std::unexpected(
        core::Error::invalid_argument("memory.remember: `importance` must be a number").with("field", "importance"));
  }
  const auto value = raw.get<double>();
  if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
    return std::unexpected(core::Error::invalid_argument("memory.remember: `importance` must be in the range [0, 1]")
                               .with("field", "importance"));
  }
  return value;
}

[[nodiscard]] core::Result<bool> parse_shadow(const json& parsed) {
  if (!parsed.contains("shadow")) {
    return false;
  }
  const auto& raw = parsed["shadow"];
  if (!raw.is_boolean()) {
    return std::unexpected(
        core::Error::invalid_argument("memory.remember: `shadow` must be a boolean").with("field", "shadow"));
  }
  return raw.get<bool>();
}

[[nodiscard]] core::Result<std::vector<std::string>> parse_string_array(const json& parsed, std::string_view field) {
  auto values = std::vector<std::string>{};
  const auto key = std::string{field};
  if (!parsed.contains(key)) {
    return values;
  }
  const auto& raw = parsed[key];
  if (!raw.is_array()) {
    return std::unexpected(core::Error::invalid_argument("memory.remember: field must be an array").with("field", key));
  }
  values.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const auto& item = raw[i];
    if (!item.is_string()) {
      return std::unexpected(core::Error::invalid_argument("memory.remember: array item must be a string")
                                 .with("field", key)
                                 .with("index", std::to_string(i)));
    }
    auto value = item.get<std::string>();
    if (value.empty()) {
      return std::unexpected(core::Error::invalid_argument("memory.remember: array item must be non-empty")
                                 .with("field", key)
                                 .with("index", std::to_string(i)));
    }
    if (std::ranges::contains(values, value)) {
      return std::unexpected(core::Error::invalid_argument("memory.remember: array values must be unique")
                                 .with("field", key)
                                 .with("value", value));
    }
    values.push_back(std::move(value));
  }
  return values;
}

[[nodiscard]] core::Result<MemoryRememberRequest> parse_remember(std::string_view input_json) {
  auto parsed = detail::parse_input_object(input_json, kMemoryRememberName);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto id = require_non_empty_string(*parsed, "id");
  if (!id) {
    return std::unexpected(std::move(id).error());
  }
  auto kind = require_non_empty_string(*parsed, "kind");
  if (!kind) {
    return std::unexpected(std::move(kind).error());
  }
  auto title = require_non_empty_string(*parsed, "title");
  if (!title) {
    return std::unexpected(std::move(title).error());
  }
  auto body = require_non_empty_string(*parsed, "body");
  if (!body) {
    return std::unexpected(std::move(body).error());
  }
  auto importance = parse_importance(*parsed);
  if (!importance) {
    return std::unexpected(std::move(importance).error());
  }
  auto tags = parse_string_array(*parsed, "tags");
  if (!tags) {
    return std::unexpected(std::move(tags).error());
  }
  auto linked_record_ids = parse_string_array(*parsed, "linked_record_ids");
  if (!linked_record_ids) {
    return std::unexpected(std::move(linked_record_ids).error());
  }
  auto shadow = parse_shadow(*parsed);
  if (!shadow) {
    return std::unexpected(std::move(shadow).error());
  }

  return MemoryRememberRequest{
      .id = std::move(*id),
      .kind = std::move(*kind),
      .title = std::move(*title),
      .body = std::move(*body),
      .importance = *importance,
      .tags = std::move(*tags),
      .linked_record_ids = std::move(*linked_record_ids),
      .shadow = *shadow,
  };
}

[[nodiscard]] async::Awaitable<core::Result<Output>> memory_remember_handler(std::string_view input_json,
                                                                             DispatchContext& ctx) {
  auto parsed = parse_remember(input_json);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed).error());
  }
  if (!ctx.memory_remember) {
    co_return std::unexpected(core::Error::invalid_argument("memory.remember: runtime service is not available")
                                  .with("reason", "memory_runtime_unavailable")
                                  .with("id", parsed->id));
  }

  co_return co_await ctx.memory_remember(std::move(*parsed), ctx);
}

}  // namespace

core::Result<void> register_memory_remember(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kMemoryRememberName},
      .description = "Create or replace one long-term memory record in the current agent scope. The runtime supplies "
                     "scope and timestamps; the result returns confirmation text plus structured saved-record "
                     "metadata.",
      .input_schema_json = std::string{kMemoryRememberSchema},
      .required_capabilities = {core::Capability::write_memory},
      .deferred = true,
      .category = "memory",
  };

  return registry.add(std::move(def), &memory_remember_handler);
}

}  // namespace orangutan::tool

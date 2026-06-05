// src/oran-tool/memory_recall.cpp - `memory.recall` built-in.

#include <oran/tool/builtins.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

constexpr std::size_t kDefaultMemoryRecallLimit = 5;
constexpr std::size_t kMaxMemoryRecallLimit = 20;

constexpr std::string_view kMemoryRecallSchema =
    R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":20},"kinds":{"type":"array","items":{"type":"string","enum":["user","feedback","project","reference","team"]},"uniqueItems":true}},"required":["query"],"additionalProperties":false})";

[[nodiscard]] core::Result<std::size_t> parse_limit(const json& parsed) {
  const auto it = parsed.find("limit");
  if (it == parsed.end()) {
    return kDefaultMemoryRecallLimit;
  }
  if (!it->is_number_unsigned()) {
    return std::unexpected(
        core::Error::invalid_argument("memory.recall: `limit` must be a positive integer").with("field", "limit"));
  }
  const auto value = it->get<std::uint64_t>();
  if (value == 0 || value > kMaxMemoryRecallLimit) {
    return std::unexpected(
        core::Error::invalid_argument("memory.recall: `limit` must be between 1 and 20").with("field", "limit"));
  }
  return static_cast<std::size_t>(value);
}

[[nodiscard]] core::Result<std::vector<std::string>> parse_kinds(const json& parsed) {
  auto kinds = std::vector<std::string>{};
  const auto it = parsed.find("kinds");
  if (it == parsed.end()) {
    return kinds;
  }
  if (!it->is_array()) {
    return std::unexpected(
        core::Error::invalid_argument("memory.recall: `kinds` must be an array").with("field", "kinds"));
  }
  kinds.reserve(it->size());
  for (std::size_t i = 0; i < it->size(); ++i) {
    const auto& item = (*it)[i];
    if (!item.is_string()) {
      return std::unexpected(core::Error::invalid_argument("memory.recall: kind must be a string")
                                 .with("field", "kinds")
                                 .with("index", std::to_string(i)));
    }
    auto kind = item.get<std::string>();
    if (kind.empty()) {
      return std::unexpected(core::Error::invalid_argument("memory.recall: kind must be non-empty")
                                 .with("field", "kinds")
                                 .with("index", std::to_string(i)));
    }
    if (std::ranges::contains(kinds, kind)) {
      return std::unexpected(core::Error::invalid_argument("memory.recall: kind filters must be unique")
                                 .with("field", "kinds")
                                 .with("kind", kind));
    }
    kinds.push_back(std::move(kind));
  }
  return kinds;
}

[[nodiscard]] core::Result<MemoryRecallRequest> parse_recall(std::string_view input_json) {
  auto parsed = detail::parse_input_object(input_json, kMemoryRecallName);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto query = detail::require_string_field(*parsed, kMemoryRecallName, "query");
  if (!query) {
    return std::unexpected(std::move(query).error());
  }
  if (query->empty()) {
    return std::unexpected(core::Error::invalid_argument("memory.recall: `query` must be non-empty"));
  }

  auto limit = parse_limit(*parsed);
  if (!limit) {
    return std::unexpected(std::move(limit).error());
  }
  auto kinds = parse_kinds(*parsed);
  if (!kinds) {
    return std::unexpected(std::move(kinds).error());
  }

  return MemoryRecallRequest{
      .query = std::move(*query),
      .limit = *limit,
      .kinds = std::move(*kinds),
  };
}

[[nodiscard]] async::Awaitable<core::Result<Output>> memory_recall_handler(std::string_view input_json,
                                                                           DispatchContext& ctx) {
  auto parsed = parse_recall(input_json);
  if (!parsed) {
    co_return std::unexpected(std::move(parsed).error());
  }
  if (!ctx.memory_recall) {
    co_return std::unexpected(core::Error::invalid_argument("memory.recall: runtime service is not available")
                                  .with("reason", "memory_runtime_unavailable"));
  }

  co_return co_await ctx.memory_recall(std::move(*parsed), ctx);
}

}  // namespace

core::Result<void> register_memory_recall(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kMemoryRecallName},
      .description = "Recall matching long-term memory records by query. Optional kind filters constrain the "
                     "search to specific record kinds; the result returns deterministic recall text plus "
                     "structured record metadata.",
      .input_schema_json = std::string{kMemoryRecallSchema},
      .required_capabilities = {core::Capability::read_memory},
      .deferred = true,
      .category = "memory",
  };

  return registry.add(std::move(def), &memory_recall_handler);
}

}  // namespace orangutan::tool

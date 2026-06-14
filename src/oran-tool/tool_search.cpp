// src/oran-tool/tool_search.cpp — `ToolSearch` built-in.
//
// This is the registry-owned half of spec 0016's deferred-tool lookup: it can
// expose full metadata for any registered tool, while the future agent session
// layer still owns per-session promotion state.

#include <oran/tool/builtins.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
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

constexpr std::string_view kToolSearchSchema =
    R"({"type":"object","properties":{"name":{"type":"string"},"category":{"type":"string"},"capability":{"type":"string"}},"anyOf":[{"required":["name"]},{"required":["category"]},{"required":["capability"]}],"additionalProperties":false})";

struct ParsedQuery {
  std::optional<std::string> name{};
  std::optional<std::string> category{};
  std::optional<std::string> capability_name{};
  std::optional<core::Capability> capability{};
};

[[nodiscard]] core::Result<void>
read_string_selector(const json& input, std::string_view field, std::optional<std::string>& output) {
  const auto key = std::string{field};
  const auto it = input.find(key);
  if (it == input.end()) {
    return {};
  }
  if (!it->is_string()) {
    return std::unexpected(core::Error::invalid_argument("ToolSearch: selector must be a string").with("field", key));
  }
  auto value = it->get<std::string>();
  if (value.empty()) {
    return std::unexpected(core::Error::invalid_argument("ToolSearch: selector must be non-empty").with("field", key));
  }
  output = std::move(value);
  return {};
}

[[nodiscard]] core::Result<ParsedQuery> parse_query(std::string_view input_json) {
  auto parsed = detail::parse_input_object(input_json, kToolSearchName);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  ParsedQuery query;
  if (auto r = read_string_selector(*parsed, "name", query.name); !r) {
    return std::unexpected(std::move(r).error());
  }
  if (auto r = read_string_selector(*parsed, "category", query.category); !r) {
    return std::unexpected(std::move(r).error());
  }
  if (auto r = read_string_selector(*parsed, "capability", query.capability_name); !r) {
    return std::unexpected(std::move(r).error());
  }

  if (!query.name.has_value() && !query.category.has_value() && !query.capability_name.has_value()) {
    return std::unexpected(
        core::Error::invalid_argument("ToolSearch: at least one of `name`, `category`, or `capability` is required"));
  }

  if (query.capability_name.has_value()) {
    auto capability = core::parse_enum<core::Capability>(*query.capability_name);
    if (!capability.has_value()) {
      return std::unexpected(
          core::Error::invalid_argument("ToolSearch: unknown capability").with("capability", *query.capability_name));
    }
    query.capability = *capability;
  }

  return query;
}

[[nodiscard]] bool matches_query(const core::ToolDef& def, const ParsedQuery& query) {
  if (query.name.has_value() && def.name != *query.name) {
    return false;
  }
  if (query.category.has_value() && (!def.category.has_value() || *def.category != *query.category)) {
    return false;
  }
  if (query.capability.has_value() && !std::ranges::contains(def.required_capabilities, *query.capability)) {
    return false;
  }
  return true;
}

[[nodiscard]] json format_query_json(const ParsedQuery& query) {
  json out = json::object();
  if (query.name.has_value()) {
    out["name"] = *query.name;
  }
  if (query.category.has_value()) {
    out["category"] = *query.category;
  }
  if (query.capability_name.has_value()) {
    out["capability"] = *query.capability_name;
  }
  return out;
}

[[nodiscard]] json format_capabilities_json(const std::vector<core::Capability>& capabilities) {
  json out = json::array();
  for (const auto capability : capabilities) {
    out.push_back(std::string{core::enum_name(capability)});
  }
  return out;
}

[[nodiscard]] json format_schema_json(const core::ToolDef& def) {
  try {
    return json::parse(def.input_schema_json);
  } catch (const std::exception&) {
    return def.input_schema_json;
  }
}

[[nodiscard]] std::string format_data_json(const ParsedQuery& query, const std::vector<core::ToolDef>& matches) {
  json out{
      {"kind", "tool_search"},
      {"query", format_query_json(query)},
      {"match_count", matches.size()},
      {"matches", json::array()},
  };

  for (const auto& def : matches) {
    json item{
        {"name", def.name},
        {"description", def.description},
        {"input_schema", format_schema_json(def)},
        {"required_capabilities", format_capabilities_json(def.required_capabilities)},
        {"deferred", def.deferred},
    };
    if (def.category.has_value()) {
      item["category"] = *def.category;
    } else {
      item["category"] = nullptr;
    }
    out["matches"].push_back(std::move(item));
  }

  return out.dump();
}

[[nodiscard]] std::string format_text(const std::vector<core::ToolDef>& matches) {
  if (matches.empty()) {
    return "ToolSearch: no matches";
  }

  std::string text;
  std::format_to(std::back_inserter(text), "ToolSearch: {} match{}", matches.size(), matches.size() == 1 ? "" : "es");
  for (const auto& def : matches) {
    std::format_to(std::back_inserter(text), "\n- {}", def.name);
    if (def.category.has_value()) {
      std::format_to(std::back_inserter(text), " [{}]", *def.category);
    }
    if (def.deferred) {
      text.append(" [deferred]");
    }
    std::format_to(std::back_inserter(text), ": {}", def.description);
  }
  return text;
}

[[nodiscard]] async::Awaitable<core::Result<Output>> tool_search_handler(std::string_view input_json,
                                                                         DispatchContext& ctx) {
  auto query = parse_query(input_json);
  if (!query) {
    co_return std::unexpected(std::move(query).error());
  }

  if (ctx.registry == nullptr) {
    co_return std::unexpected(core::Error::internal("ToolSearch: registry context is not available"));
  }

  std::vector<core::ToolDef> matches;
  const auto catalog = ctx.registry->catalog();
  matches.reserve(catalog.size());
  for (const auto& def : catalog) {
    if (matches_query(def, *query)) {
      matches.push_back(def);
    }
  }

  co_return Output{
      .text = format_text(matches),
      .data_json = format_data_json(*query, matches),
      .usage =
          ToolUsage{
              .match_count = static_cast<std::uint64_t>(matches.size()),
          },
  };
}

}  // namespace

core::Result<void> register_tool_search(Registry& registry) {
  core::ToolDef def{
      .name = std::string{kToolSearchName},
      .description = "Search the current tool registry metadata by exact name, category, and/or required "
                     "capability. At least one selector is required; supplied selectors are ANDed. Returns a "
                     "text summary plus structured metadata with the matched tool definitions.",
      .input_schema_json = std::string{kToolSearchSchema},
      .required_capabilities = {},
      .deferred = false,
      .category = "runtime",
  };

  return registry.add(std::move(def), &tool_search_handler);
}

}  // namespace orangutan::tool

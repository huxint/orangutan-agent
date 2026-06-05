// src/oran-config/config.cpp — JSON-backed oran-config loader.

#include <oran/config/config.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>
#include <re2/re2.h>

#include <oran/core/capability.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>

namespace orangutan::config {
namespace {

using json = ::nlohmann::ordered_json;
using ::orangutan::core::Error;
using ::orangutan::core::Result;

constexpr auto kRecognizedRootFields = std::array<std::string_view, 14>{
    "strict_config",
    "runtime",
    "trace",
    "permissions",
    "profiles",
    "routes",
    "agents",
    "teams",
    "channels",
    "hooks",
    "memory",
    "automation",
    "web",
    "session",
};

constexpr auto kRecognizedAgentFields = std::array<std::string_view, 5>{
    "permissions",
    "prompt_overlay",
    "skills_enabled",
    "skills_deactivated",
    "skills_expirations",
};

constexpr auto kRecognizedHookFields = std::array<std::string_view, 3>{
    "timeout_ms",
    "sinks",
    "bindings",
};

constexpr auto kRecognizedMemoryFields = std::array<std::string_view, 1>{
    "longterm",
};

constexpr auto kRecognizedLongtermMemoryFields = std::array<std::string_view, 2>{
    "recall",
    "hybrid_search",
};

constexpr auto kRecognizedLongtermRecallFields = std::array<std::string_view, 4>{
    "enabled",
    "limit",
    "query_strategy",
    "kinds",
};

constexpr auto kRecognizedLongtermHybridSearchFields = std::array<std::string_view, 6>{
    "enabled",
    "lexical_limit",
    "vector_limit",
    "result_limit",
    "lexical_weight",
    "vector_weight",
};

constexpr auto kRecognizedProfileFields = std::array<std::string_view, 6>{
    "provider",
    "protocol",
    "model",
    "base_url",
    "api_key_env",
    "pricing",
};

constexpr auto kRecognizedProviderPricingFields = std::array<std::string_view, 4>{
    "input_per_million_usd",
    "output_per_million_usd",
    "cache_creation_per_million_usd",
    "cache_read_per_million_usd",
};

constexpr auto kRecognizedRouteFields = std::array<std::string_view, 2>{
    "primary",
    "fallbacks",
};

[[nodiscard]] Error config_error(std::string message, std::string path) {
  return Error::config(std::move(message)).with("path", std::move(path));
}

[[nodiscard]] std::string child_path(std::string_view base, std::string_view child) {
  if (base.empty() || base == "$") {
    return std::format("$.{}", child);
  }
  return std::format("{}.{}", base, child);
}

[[nodiscard]] std::string element_path(std::string_view base, std::size_t index) {
  return std::format("{}[{}]", base, index);
}

[[nodiscard]] bool is_recognized_root(std::string_view name) {
  return std::ranges::contains(kRecognizedRootFields, name);
}

[[nodiscard]] bool is_recognized_agent_field(std::string_view name) {
  return std::ranges::contains(kRecognizedAgentFields, name);
}

template <std::size_t N>
[[nodiscard]] Result<void> collect_unknown_object_fields(const json& object,
                                                         std::string_view path,
                                                         const std::array<std::string_view, N>& known_keys,
                                                         std::string_view message,
                                                         bool strict,
                                                         std::vector<ConfigWarning>& warnings) {
  for (const auto& [key, _] : object.items()) {
    if (std::ranges::contains(known_keys, key)) {
      continue;
    }
    const auto field_path = child_path(path, key);
    if (strict) {
      return std::unexpected(config_error(std::string{message}, field_path));
    }
    warnings.push_back(ConfigWarning{
        .path = field_path,
        .message = std::string{message},
    });
  }
  return {};
}

[[nodiscard]] bool valid_env_name(std::string_view name) {
  return !name.empty() &&
         std::ranges::all_of(name, [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_'; });
}

[[nodiscard]] Result<std::string> expand_env_string(std::string_view input, std::string_view path) {
  auto output = std::string{};
  auto cursor = std::size_t{0};

  while (cursor < input.size()) {
    const auto start = input.find("${", cursor);
    if (start == std::string_view::npos) {
      output.append(input.substr(cursor));
      break;
    }

    output.append(input.substr(cursor, start - cursor));
    const auto end = input.find('}', start + 2);
    if (end == std::string_view::npos) {
      return std::unexpected(config_error("unterminated environment substitution", std::string{path}));
    }

    const auto expr = input.substr(start + 2, end - start - 2);
    const auto default_marker = expr.find(":-");
    const auto has_default = default_marker != std::string_view::npos;
    const auto name = has_default ? expr.substr(0, default_marker) : expr;
    const auto fallback = has_default ? expr.substr(default_marker + 2) : std::string_view{};
    if (!valid_env_name(name)) {
      return std::unexpected(
          config_error("invalid environment variable name", std::string{path}).with("name", std::string{name}));
    }

    const auto name_text = std::string{name};
    const auto* value = std::getenv(name_text.c_str());
    if (value != nullptr && (value[0] != '\0' || !has_default)) {
      output.append(value);
    } else if (has_default) {
      output.append(fallback);
    } else {
      return std::unexpected(config_error("missing environment variable", std::string{path}).with("name", name_text));
    }

    cursor = end + 1;
  }

  return output;
}

[[nodiscard]] Result<void> substitute_env(json& value, std::string_view path) {
  if (value.is_string()) {
    auto expanded = expand_env_string(value.get_ref<const std::string&>(), path);
    if (!expanded) {
      return std::unexpected(std::move(expanded.error()));
    }
    value = std::move(*expanded);
    return {};
  }

  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      auto result = substitute_env(it.value(), child_path(path, it.key()));
      if (!result) {
        return std::unexpected(std::move(result.error()));
      }
    }
    return {};
  }

  if (value.is_array()) {
    auto index = std::size_t{0};
    for (auto& item : value) {
      auto result = substitute_env(item, element_path(path, index));
      if (!result) {
        return std::unexpected(std::move(result.error()));
      }
      ++index;
    }
  }

  return {};
}

[[nodiscard]] Result<void> substitute_env_recognized_roots(json& root) {
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (!is_recognized_root(it.key())) {
      continue;
    }

    auto result = substitute_env(it.value(), child_path("$", it.key()));
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
  }
  return {};
}

[[nodiscard]] Result<void> require_object(const json& value, std::string_view path) {
  if (!value.is_object()) {
    return std::unexpected(config_error("expected object", std::string{path}));
  }
  return {};
}

[[nodiscard]] Result<std::string> required_string(const json& object, std::string_view key, std::string_view path) {
  const auto it = object.find(key);
  if (it == object.end()) {
    return std::unexpected(config_error("missing required string field", child_path(path, key)));
  }
  if (!it->is_string()) {
    return std::unexpected(config_error("expected string", child_path(path, key)));
  }
  return it->get<std::string>();
}

[[nodiscard]] Result<std::vector<std::string>> string_array(const json& value, std::string_view path) {
  if (!value.is_array()) {
    return std::unexpected(config_error("expected array", std::string{path}));
  }

  auto out = std::vector<std::string>{};
  out.reserve(value.size());
  auto index = std::size_t{0};
  for (const auto& item : value) {
    if (!item.is_string()) {
      return std::unexpected(config_error("expected string", element_path(path, index)));
    }
    out.push_back(item.get<std::string>());
    ++index;
  }
  return out;
}

[[nodiscard]] Result<std::int64_t> integer_value(const json& value, std::string_view path) {
  if (!value.is_number_integer()) {
    return std::unexpected(config_error("expected integer", std::string{path}));
  }
  if (value.is_number_unsigned()) {
    const auto unsigned_value = value.get<std::uint64_t>();
    if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return std::unexpected(config_error("integer is out of range", std::string{path}));
    }
    return static_cast<std::int64_t>(unsigned_value);
  }
  return value.get<std::int64_t>();
}

[[nodiscard]] Result<std::int64_t> positive_integer_value(const json& value, std::string_view path) {
  auto parsed = integer_value(value, path);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (*parsed <= 0) {
    return std::unexpected(config_error("expected positive integer", std::string{path}));
  }
  return parsed;
}

[[nodiscard]] Result<double> non_negative_number_value(const json& value, std::string_view path) {
  if (!value.is_number()) {
    return std::unexpected(config_error("expected number", std::string{path}));
  }
  const auto parsed = value.get<double>();
  if (!std::isfinite(parsed) || parsed < 0.0) {
    return std::unexpected(config_error("expected non-negative finite number", std::string{path}));
  }
  return parsed;
}

[[nodiscard]] Result<bool> parse_strict_config(const json& root) {
  const auto it = root.find("strict_config");
  if (it == root.end()) {
    return false;
  }
  if (!it->is_boolean()) {
    return std::unexpected(config_error("expected boolean", "$.strict_config"));
  }
  return it->get<bool>();
}

[[nodiscard]] Result<ToolOutputRuntimeConfig> parse_tool_output_runtime(const json& runtime) {
  auto config = ToolOutputRuntimeConfig{};
  const auto it = runtime.find("tool_output");
  if (it == runtime.end()) {
    return config;
  }
  auto object = require_object(*it, "$.runtime.tool_output");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  if (const auto max_text = it->find("max_text_bytes"); max_text != it->end()) {
    auto parsed = positive_integer_value(*max_text, "$.runtime.tool_output.max_text_bytes");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    config.max_text_bytes = *parsed;
  }

  if (const auto max_data = it->find("max_data_bytes"); max_data != it->end()) {
    auto parsed = positive_integer_value(*max_data, "$.runtime.tool_output.max_data_bytes");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    config.max_data_bytes = *parsed;
  }

  return config;
}

[[nodiscard]] Result<ToolSchedulerRuntimeConfig> parse_tool_scheduler_runtime(const json& runtime) {
  auto config = ToolSchedulerRuntimeConfig{};
  const auto it = runtime.find("tool_scheduler");
  if (it == runtime.end()) {
    return config;
  }
  auto object = require_object(*it, "$.runtime.tool_scheduler");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  if (const auto max_parallel = it->find("max_parallel_tools"); max_parallel != it->end()) {
    auto parsed = positive_integer_value(*max_parallel, "$.runtime.tool_scheduler.max_parallel_tools");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    config.max_parallel_tools = *parsed;
  }

  if (const auto per_call = it->find("per_call_timeout_ms"); per_call != it->end()) {
    auto parsed = positive_integer_value(*per_call, "$.runtime.tool_scheduler.per_call_timeout_ms");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    config.per_call_timeout_ms = *parsed;
  }

  if (const auto idle_ttl = it->find("idle_lock_ttl_ms"); idle_ttl != it->end()) {
    auto parsed = positive_integer_value(*idle_ttl, "$.runtime.tool_scheduler.idle_lock_ttl_ms");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    config.idle_lock_ttl_ms = *parsed;
  }

  return config;
}

[[nodiscard]] Result<PromptActiveToolsConfig> parse_prompt_active_tools(const json& value, std::string_view path) {
  auto config = PromptActiveToolsConfig{};
  if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    if (text != "defaults") {
      return std::unexpected(
          config_error("expected \"defaults\" or array of tool names", std::string{path}).with("value", text));
    }
    return config;
  }

  auto parsed = string_array(value, path);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }

  auto index = std::size_t{0};
  for (const auto& name : *parsed) {
    if (name.empty()) {
      return std::unexpected(config_error("tool name must be non-empty", element_path(path, index)));
    }
    ++index;
  }

  config.use_defaults = false;
  config.tool_names = std::move(*parsed);
  return config;
}

[[nodiscard]] Result<std::vector<std::string>>
parse_non_empty_string_array(const json& value, std::string_view path, std::string_view item_name) {
  auto parsed = string_array(value, path);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }

  auto index = std::size_t{0};
  for (const auto& name : *parsed) {
    if (name.empty()) {
      return std::unexpected(
          config_error(std::string{item_name}.append(" must be non-empty"), element_path(path, index)));
    }
    ++index;
  }
  return parsed;
}

[[nodiscard]] Result<std::vector<SkillExpirationConfig>> parse_skill_expirations(const json& value,
                                                                                 std::string_view path) {
  if (!value.is_array()) {
    return std::unexpected(config_error("expected array of skill expirations", std::string{path}));
  }

  auto expirations = std::vector<SkillExpirationConfig>{};
  expirations.reserve(value.size());
  auto index = std::size_t{0};
  for (const auto& element : value) {
    const auto entry_path = element_path(path, index);
    ++index;
    if (!element.is_object()) {
      return std::unexpected(config_error("skill expiration must be an object", entry_path));
    }

    const auto name_it = element.find("name");
    if (name_it == element.end() || !name_it->is_string()) {
      return std::unexpected(config_error("skill expiration requires a string `name`", child_path(entry_path, "name")));
    }
    auto name = name_it->get<std::string>();
    if (name.empty()) {
      return std::unexpected(config_error("skill expiration name must be non-empty", child_path(entry_path, "name")));
    }

    const auto expires_it = element.find("expires_at");
    if (expires_it == element.end() || !expires_it->is_string()) {
      return std::unexpected(
          config_error("skill expiration requires a string `expires_at`", child_path(entry_path, "expires_at")));
    }
    auto expires_at = core::time::parse_iso8601_utc(expires_it->get<std::string>());
    if (!expires_at) {
      return std::unexpected(config_error("skill expiration `expires_at` must be a UTC ISO-8601 timestamp",
                                          child_path(entry_path, "expires_at")));
    }

    expirations.push_back(SkillExpirationConfig{.name = std::move(name), .expires_at = *expires_at});
  }
  return expirations;
}

[[nodiscard]] Result<PromptRuntimeConfig> parse_prompt_runtime(const json& runtime) {
  auto prompt = PromptRuntimeConfig{};
  const auto it = runtime.find("prompt");
  if (it == runtime.end()) {
    return prompt;
  }
  auto object = require_object(*it, "$.runtime.prompt");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  if (const auto active_tools = it->find("active_tools"); active_tools != it->end()) {
    auto parsed = parse_prompt_active_tools(*active_tools, "$.runtime.prompt.active_tools");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    prompt.active_tools = std::move(*parsed);
  }

  return prompt;
}

[[nodiscard]] Result<RuntimeConfig> parse_runtime(const json& root) {
  auto runtime = RuntimeConfig{};
  const auto it = root.find("runtime");
  if (it == root.end()) {
    return runtime;
  }
  auto object = require_object(*it, "$.runtime");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  if (const auto workers = it->find("workers"); workers != it->end()) {
    auto parsed = integer_value(*workers, "$.runtime.workers");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    runtime.workers = *parsed;
    if (runtime.workers <= 0) {
      return std::unexpected(config_error("expected positive integer", "$.runtime.workers"));
    }
  }

  if (const auto timeout = it->find("request_timeout_ms"); timeout != it->end()) {
    auto parsed = integer_value(*timeout, "$.runtime.request_timeout_ms");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    runtime.request_timeout_ms = *parsed;
    if (runtime.request_timeout_ms <= 0) {
      return std::unexpected(config_error("expected positive integer", "$.runtime.request_timeout_ms"));
    }
  }

  if (auto tool_output = parse_tool_output_runtime(*it); !tool_output) {
    return std::unexpected(std::move(tool_output.error()));
  } else {
    runtime.tool_output = *tool_output;
  }

  if (auto tool_scheduler = parse_tool_scheduler_runtime(*it); !tool_scheduler) {
    return std::unexpected(std::move(tool_scheduler.error()));
  } else {
    runtime.tool_scheduler = *tool_scheduler;
  }

  if (auto prompt = parse_prompt_runtime(*it); !prompt) {
    return std::unexpected(std::move(prompt.error()));
  } else {
    runtime.prompt = std::move(*prompt);
  }

  if (const auto patterns = it->find("redaction_patterns"); patterns != it->end()) {
    auto parsed = string_array(*patterns, "$.runtime.redaction_patterns");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    runtime.redaction_patterns = std::move(*parsed);
  }

  return runtime;
}

[[nodiscard]] Result<TraceConfig> parse_trace(const json& root) {
  auto trace = TraceConfig{};
  const auto it = root.find("trace");
  if (it == root.end()) {
    return trace;
  }
  auto object = require_object(*it, "$.trace");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  if (const auto enabled = it->find("enabled"); enabled != it->end()) {
    if (!enabled->is_boolean()) {
      return std::unexpected(config_error("expected boolean", "$.trace.enabled"));
    }
    trace.enabled = enabled->get<bool>();
  }

  if (const auto store_raw_bodies = it->find("store_raw_bodies"); store_raw_bodies != it->end()) {
    if (!store_raw_bodies->is_boolean()) {
      return std::unexpected(config_error("expected boolean", "$.trace.store_raw_bodies"));
    }
    trace.store_raw_bodies = store_raw_bodies->get<bool>();
  }

  if (const auto retention_days = it->find("retention_days"); retention_days != it->end()) {
    auto parsed = positive_integer_value(*retention_days, "$.trace.retention_days");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    trace.retention_days = *parsed;
  }

  return trace;
}

[[nodiscard]] Result<HooksConfig> parse_hooks(const json& root, bool strict, std::vector<ConfigWarning>& warnings) {
  auto hooks = HooksConfig{};
  const auto it = root.find("hooks");
  if (it == root.end()) {
    return hooks;
  }
  auto object = require_object(*it, "$.hooks");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  if (const auto timeout = it->find("timeout_ms"); timeout != it->end()) {
    auto parsed = positive_integer_value(*timeout, "$.hooks.timeout_ms");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    hooks.timeout_ms = *parsed;
  }

  auto unknowns =
      collect_unknown_object_fields(*it, "$.hooks", kRecognizedHookFields, "unknown hook field", strict, warnings);
  if (!unknowns) {
    return std::unexpected(std::move(unknowns.error()));
  }

  return hooks;
}

[[nodiscard]] Result<LongtermMemoryRecallConfig>
parse_longterm_recall(const json& longterm, bool strict, std::vector<ConfigWarning>& warnings) {
  auto recall = LongtermMemoryRecallConfig{};
  const auto it = longterm.find("recall");
  if (it == longterm.end()) {
    return recall;
  }
  auto object = require_object(*it, "$.memory.longterm.recall");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  if (const auto enabled = it->find("enabled"); enabled != it->end()) {
    if (!enabled->is_boolean()) {
      return std::unexpected(config_error("expected boolean", "$.memory.longterm.recall.enabled"));
    }
    recall.enabled = enabled->get<bool>();
  }

  if (const auto limit = it->find("limit"); limit != it->end()) {
    auto parsed = positive_integer_value(*limit, "$.memory.longterm.recall.limit");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    recall.limit = *parsed;
  }

  if (const auto strategy = it->find("query_strategy"); strategy != it->end()) {
    constexpr std::string_view kPath = "$.memory.longterm.recall.query_strategy";
    if (!strategy->is_string()) {
      return std::unexpected(config_error("expected string", std::string{kPath}));
    }
    auto text = strategy->get<std::string>();
    auto parsed = core::parse_enum<LongtermMemoryRecallQueryStrategy>(text);
    if (!parsed) {
      return std::unexpected(config_error("unknown long-term memory recall query strategy", std::string{kPath})
                                 .with("value", std::move(text)));
    }
    recall.query_strategy = *parsed;
  }

  if (const auto kinds = it->find("kinds"); kinds != it->end()) {
    auto parsed = parse_non_empty_string_array(*kinds, "$.memory.longterm.recall.kinds", "memory kind");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    if (parsed->empty()) {
      return std::unexpected(
          config_error("long-term memory recall kinds must not be empty", "$.memory.longterm.recall.kinds"));
    }
    auto seen = std::vector<std::string>{};
    seen.reserve(parsed->size());
    for (std::size_t i = 0; i < parsed->size(); ++i) {
      const auto& name = (*parsed)[i];
      if (std::ranges::contains(seen, name)) {
        return std::unexpected(config_error("long-term memory recall kind must be unique",
                                            element_path("$.memory.longterm.recall.kinds", i)));
      }
      seen.push_back(name);
    }
    recall.kinds = std::move(*parsed);
  }

  auto unknowns = collect_unknown_object_fields(*it,
                                                "$.memory.longterm.recall",
                                                kRecognizedLongtermRecallFields,
                                                "unknown long-term memory recall field",
                                                strict,
                                                warnings);
  if (!unknowns) {
    return std::unexpected(std::move(unknowns.error()));
  }

  return recall;
}

[[nodiscard]] Result<void>
parse_optional_positive_integer(const json& object, std::string_view key, std::string_view path, std::int64_t& out) {
  const auto it = object.find(key);
  if (it == object.end()) {
    return {};
  }
  auto parsed = positive_integer_value(*it, child_path(path, key));
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  out = *parsed;
  return {};
}

[[nodiscard]] Result<void>
parse_optional_non_negative_number(const json& object, std::string_view key, std::string_view path, double& out) {
  const auto it = object.find(key);
  if (it == object.end()) {
    return {};
  }
  auto parsed = non_negative_number_value(*it, child_path(path, key));
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  out = *parsed;
  return {};
}

[[nodiscard]] Result<LongtermMemoryHybridSearchConfig>
parse_longterm_hybrid_search(const json& longterm, bool strict, std::vector<ConfigWarning>& warnings) {
  auto hybrid = LongtermMemoryHybridSearchConfig{};
  const auto it = longterm.find("hybrid_search");
  if (it == longterm.end()) {
    return hybrid;
  }
  constexpr std::string_view kPath = "$.memory.longterm.hybrid_search";
  auto object = require_object(*it, kPath);
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  if (const auto enabled = it->find("enabled"); enabled != it->end()) {
    if (!enabled->is_boolean()) {
      return std::unexpected(config_error("expected boolean", child_path(kPath, "enabled")));
    }
    hybrid.enabled = enabled->get<bool>();
  }

  if (auto parsed = parse_optional_positive_integer(*it, "lexical_limit", kPath, hybrid.lexical_limit); !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (auto parsed = parse_optional_positive_integer(*it, "vector_limit", kPath, hybrid.vector_limit); !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (auto parsed = parse_optional_positive_integer(*it, "result_limit", kPath, hybrid.result_limit); !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (auto parsed = parse_optional_non_negative_number(*it, "lexical_weight", kPath, hybrid.lexical_weight); !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (auto parsed = parse_optional_non_negative_number(*it, "vector_weight", kPath, hybrid.vector_weight); !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (hybrid.lexical_weight == 0.0 && hybrid.vector_weight == 0.0) {
    return std::unexpected(
        config_error("long-term memory hybrid search requires a non-zero weight", child_path(kPath, "weights")));
  }

  auto unknowns = collect_unknown_object_fields(*it,
                                                kPath,
                                                kRecognizedLongtermHybridSearchFields,
                                                "unknown long-term memory hybrid search field",
                                                strict,
                                                warnings);
  if (!unknowns) {
    return std::unexpected(std::move(unknowns.error()));
  }

  return hybrid;
}

[[nodiscard]] Result<LongtermMemoryConfig>
parse_longterm_memory(const json& memory, bool strict, std::vector<ConfigWarning>& warnings) {
  auto longterm = LongtermMemoryConfig{};
  const auto it = memory.find("longterm");
  if (it == memory.end()) {
    return longterm;
  }
  auto object = require_object(*it, "$.memory.longterm");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  auto recall = parse_longterm_recall(*it, strict, warnings);
  if (!recall) {
    return std::unexpected(std::move(recall.error()));
  }
  longterm.recall = *recall;

  auto hybrid = parse_longterm_hybrid_search(*it, strict, warnings);
  if (!hybrid) {
    return std::unexpected(std::move(hybrid.error()));
  }
  longterm.hybrid_search = *hybrid;

  auto unknowns = collect_unknown_object_fields(*it,
                                                "$.memory.longterm",
                                                kRecognizedLongtermMemoryFields,
                                                "unknown long-term memory field",
                                                strict,
                                                warnings);
  if (!unknowns) {
    return std::unexpected(std::move(unknowns.error()));
  }

  return longterm;
}

[[nodiscard]] Result<MemoryConfig> parse_memory(const json& root, bool strict, std::vector<ConfigWarning>& warnings) {
  auto memory = MemoryConfig{};
  const auto it = root.find("memory");
  if (it == root.end()) {
    return memory;
  }
  auto object = require_object(*it, "$.memory");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  auto longterm = parse_longterm_memory(*it, strict, warnings);
  if (!longterm) {
    return std::unexpected(std::move(longterm.error()));
  }
  memory.longterm = *longterm;

  auto unknowns =
      collect_unknown_object_fields(*it, "$.memory", kRecognizedMemoryFields, "unknown memory field", strict, warnings);
  if (!unknowns) {
    return std::unexpected(std::move(unknowns.error()));
  }

  return memory;
}

[[nodiscard]] Result<void>
parse_optional_price(const json& object, std::string_view key, std::string_view path, std::optional<double>& out) {
  const auto it = object.find(key);
  if (it == object.end()) {
    return {};
  }
  auto parsed = non_negative_number_value(*it, child_path(path, key));
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  out = *parsed;
  return {};
}

[[nodiscard]] Result<ProviderPricingConfig> parse_profile_pricing(const json& profile,
                                                                  std::string_view profile_path,
                                                                  bool strict,
                                                                  std::vector<ConfigWarning>& warnings) {
  auto pricing = ProviderPricingConfig{};
  const auto it = profile.find("pricing");
  if (it == profile.end()) {
    return pricing;
  }
  auto object = require_object(*it, child_path(profile_path, "pricing"));
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  const auto pricing_path = child_path(profile_path, "pricing");
  if (auto parsed = parse_optional_price(*it, "input_per_million_usd", pricing_path, pricing.input_per_million_usd);
      !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (auto parsed = parse_optional_price(*it, "output_per_million_usd", pricing_path, pricing.output_per_million_usd);
      !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (auto parsed = parse_optional_price(*it,
                                         "cache_creation_per_million_usd",
                                         pricing_path,
                                         pricing.cache_creation_per_million_usd);
      !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (auto parsed =
          parse_optional_price(*it, "cache_read_per_million_usd", pricing_path, pricing.cache_read_per_million_usd);
      !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  auto unknowns = collect_unknown_object_fields(*it,
                                                pricing_path,
                                                kRecognizedProviderPricingFields,
                                                "unknown provider pricing field",
                                                strict,
                                                warnings);
  if (!unknowns) {
    return std::unexpected(std::move(unknowns.error()));
  }
  return pricing;
}

[[nodiscard]] Result<std::vector<ProfileConfig>>
parse_profiles(const json& root, bool strict, std::vector<ConfigWarning>& warnings) {
  auto profiles = std::vector<ProfileConfig>{};
  const auto it = root.find("profiles");
  if (it == root.end()) {
    return profiles;
  }
  auto object = require_object(*it, "$.profiles");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  profiles.reserve(it->size());
  for (const auto& [name, value] : it->items()) {
    const auto profile_path = child_path("$.profiles", name);
    auto profile_object = require_object(value, profile_path);
    if (!profile_object) {
      return std::unexpected(std::move(profile_object.error()));
    }

    auto provider = required_string(value, "provider", profile_path);
    if (!provider) {
      return std::unexpected(std::move(provider.error()));
    }
    auto protocol = std::optional<std::string>{};
    if (const auto protocol_it = value.find("protocol"); protocol_it != value.end()) {
      if (!protocol_it->is_string()) {
        return std::unexpected(config_error("expected string", child_path(profile_path, "protocol")));
      }
      protocol = protocol_it->get<std::string>();
      if (protocol->empty()) {
        return std::unexpected(config_error("protocol must be non-empty", child_path(profile_path, "protocol")));
      }
    }
    auto model = required_string(value, "model", profile_path);
    if (!model) {
      return std::unexpected(std::move(model.error()));
    }
    auto base_url = required_string(value, "base_url", profile_path);
    if (!base_url) {
      return std::unexpected(std::move(base_url.error()));
    }
    auto api_key_env = required_string(value, "api_key_env", profile_path);
    if (!api_key_env) {
      return std::unexpected(std::move(api_key_env.error()));
    }
    auto pricing = parse_profile_pricing(value, profile_path, strict, warnings);
    if (!pricing) {
      return std::unexpected(std::move(pricing.error()));
    }
    auto unknowns = collect_unknown_object_fields(value,
                                                  profile_path,
                                                  kRecognizedProfileFields,
                                                  "unknown provider profile field",
                                                  strict,
                                                  warnings);
    if (!unknowns) {
      return std::unexpected(std::move(unknowns.error()));
    }

    profiles.push_back(ProfileConfig{
        .name = name,
        .provider = std::move(*provider),
        .protocol = std::move(protocol),
        .model = std::move(*model),
        .base_url = std::move(*base_url),
        .api_key_env = std::move(*api_key_env),
        .pricing = *pricing,
    });
  }

  return profiles;
}

[[nodiscard]] Result<std::vector<RouteConfig>>
parse_routes(const json& root, bool strict, std::vector<ConfigWarning>& warnings) {
  auto routes = std::vector<RouteConfig>{};
  const auto it = root.find("routes");
  if (it == root.end()) {
    return routes;
  }
  auto object = require_object(*it, "$.routes");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  routes.reserve(it->size());
  for (const auto& [name, value] : it->items()) {
    const auto route_path = child_path("$.routes", name);
    auto route_object = require_object(value, route_path);
    if (!route_object) {
      return std::unexpected(std::move(route_object.error()));
    }

    auto primary = required_string(value, "primary", route_path);
    if (!primary) {
      return std::unexpected(std::move(primary.error()));
    }

    auto fallbacks = std::vector<std::string>{};
    if (const auto fallback_it = value.find("fallbacks"); fallback_it != value.end()) {
      auto parsed = string_array(*fallback_it, child_path(route_path, "fallbacks"));
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      fallbacks = std::move(*parsed);
    }

    auto unknowns = collect_unknown_object_fields(value,
                                                  route_path,
                                                  kRecognizedRouteFields,
                                                  "unknown route field",
                                                  strict,
                                                  warnings);
    if (!unknowns) {
      return std::unexpected(std::move(unknowns.error()));
    }

    routes.push_back(RouteConfig{
        .name = name,
        .primary_profile = std::move(*primary),
        .fallback_profiles = std::move(fallbacks),
    });
  }

  return routes;
}

[[nodiscard]] Result<SessionConfig> parse_session(const json& root) {
  auto session = SessionConfig{};
  const auto it = root.find("session");
  if (it == root.end()) {
    return session;
  }
  auto object = require_object(*it, "$.session");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  if (const auto auto_save = it->find("auto_save"); auto_save != it->end()) {
    if (!auto_save->is_boolean()) {
      return std::unexpected(config_error("expected boolean", "$.session.auto_save"));
    }
    session.auto_save = auto_save->get<bool>();
  }

  if (const auto persistence = it->find("persistence"); persistence != it->end()) {
    if (!persistence->is_boolean()) {
      return std::unexpected(config_error("expected boolean", "$.session.persistence"));
    }
    session.persistence = persistence->get<bool>();
  }

  return session;
}

[[nodiscard]] Result<WebConfig> parse_web(const json& root) {
  auto web = WebConfig{};
  const auto it = root.find("web");
  if (it == root.end()) {
    return web;
  }
  auto object = require_object(*it, "$.web");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  if (const auto enabled = it->find("enabled"); enabled != it->end()) {
    if (!enabled->is_boolean()) {
      return std::unexpected(config_error("expected boolean", "$.web.enabled"));
    }
    web.enabled = enabled->get<bool>();
  }

  if (const auto bind = it->find("bind"); bind != it->end()) {
    if (!bind->is_string()) {
      return std::unexpected(config_error("expected string", "$.web.bind"));
    }
    web.bind = bind->get<std::string>();
  }

  if (const auto port = it->find("port"); port != it->end()) {
    auto parsed = integer_value(*port, "$.web.port");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    web.port = *parsed;
    if (web.port <= 0 || web.port > std::numeric_limits<std::uint16_t>::max()) {
      return std::unexpected(config_error("expected TCP port in range 1..65535", "$.web.port"));
    }
  }

  return web;
}

[[nodiscard]] Result<std::vector<ConfigWarning>> collect_unknown_root_fields(const json& root, bool strict) {
  auto warnings = std::vector<ConfigWarning>{};
  for (const auto& [key, value] : root.items()) {
    static_cast<void>(value);
    if (is_recognized_root(key)) {
      continue;
    }
    const auto path = child_path("$", key);
    if (strict) {
      return std::unexpected(config_error("unknown root config field", path));
    }
    warnings.push_back(ConfigWarning{
        .path = path,
        .message = "unknown root config field",
    });
  }
  return warnings;
}

[[nodiscard]] Result<PermissionRuleConfig> parse_permission_rule(const json& value,
                                                                 PermissionVerdict verdict,
                                                                 std::string_view path,
                                                                 bool strict,
                                                                 std::vector<ConfigWarning>& warnings) {
  auto rule_object = require_object(value, path);
  if (!rule_object) {
    return std::unexpected(std::move(rule_object.error()));
  }

  auto tool_pattern = required_string(value, "tool_pattern", path);
  if (!tool_pattern) {
    return std::unexpected(std::move(tool_pattern.error()));
  }
  if (tool_pattern->empty()) {
    return std::unexpected(config_error("tool_pattern must be non-empty", child_path(path, "tool_pattern")));
  }

  auto capability = std::optional<core::Capability>{};
  if (const auto cap_it = value.find("capability"); cap_it != value.end()) {
    if (!cap_it->is_string()) {
      return std::unexpected(config_error("expected string", child_path(path, "capability")));
    }
    const auto& text = cap_it->get_ref<const std::string&>();
    auto parsed = core::parse_enum<core::Capability>(text);
    if (!parsed) {
      return std::unexpected(
          config_error("unknown capability spelling", child_path(path, "capability")).with("capability", text));
    }
    capability = *parsed;
  }

  auto input_pattern = std::optional<std::string>{};
  if (const auto pat_it = value.find("input_pattern"); pat_it != value.end()) {
    if (!pat_it->is_string()) {
      return std::unexpected(config_error("expected string", child_path(path, "input_pattern")));
    }
    auto pattern = pat_it->get_ref<const std::string&>();
    if (pattern.empty()) {
      return std::unexpected(config_error("input_pattern must be non-empty", child_path(path, "input_pattern")));
    }
    // Validate the pattern by compiling re2 once with logging disabled.
    // The compiled regex is intentionally discarded; the matching slice in
    // `oran-permission::materialize` recompiles the source string when
    // assembling the runtime `Rule` (the re-compile cost is microseconds,
    // and keeping config copyable matters more than saving one compile).
    re2::RE2::Options options{re2::RE2::DefaultOptions};
    options.set_log_errors(false);
    const auto compiled = re2::RE2{pattern, options};
    if (!compiled.ok()) {
      return std::unexpected(config_error("invalid input_pattern regex", child_path(path, "input_pattern"))
                                 .with("regex_error", compiled.error()));
    }
    input_pattern = std::move(pattern);
  }

  auto replay_max = std::optional<std::uint32_t>{};
  if (const auto it = value.find("replay_max"); it != value.end()) {
    auto parsed = integer_value(*it, child_path(path, "replay_max"));
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    if (*parsed < 0) {
      return std::unexpected(config_error("replay_max must be non-negative", child_path(path, "replay_max"))
                                 .with("value", std::to_string(*parsed)));
    }
    if (*parsed > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
      return std::unexpected(config_error("replay_max exceeds uint32 range", child_path(path, "replay_max"))
                                 .with("value", std::to_string(*parsed)));
    }
    replay_max = static_cast<std::uint32_t>(*parsed);
  }

  auto approval_ttl_seconds = std::optional<std::int64_t>{};
  if (const auto it = value.find("approval_ttl_seconds"); it != value.end()) {
    auto parsed = integer_value(*it, child_path(path, "approval_ttl_seconds"));
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    if (*parsed < 0) {
      return std::unexpected(
          config_error("approval_ttl_seconds must be non-negative", child_path(path, "approval_ttl_seconds"))
              .with("value", std::to_string(*parsed)));
    }
    approval_ttl_seconds = *parsed;
  }

  static constexpr auto kKnownKeys = std::array<std::string_view, 5>{"tool_pattern",
                                                                     "capability",
                                                                     "input_pattern",
                                                                     "replay_max",
                                                                     "approval_ttl_seconds"};
  for (const auto& [key, _] : value.items()) {
    if (std::ranges::contains(kKnownKeys, key)) {
      continue;
    }
    const auto field_path = child_path(path, key);
    if (strict) {
      return std::unexpected(config_error("unknown permission rule field", field_path));
    }
    warnings.push_back(ConfigWarning{
        .path = field_path,
        .message = "unknown permission rule field",
    });
  }

  return PermissionRuleConfig{
      .verdict = verdict,
      .tool_pattern = std::move(*tool_pattern),
      .capability = capability,
      .input_pattern = std::move(input_pattern),
      .replay_max = replay_max,
      .approval_ttl_seconds = approval_ttl_seconds,
  };
}

[[nodiscard]] Result<WorkspacePermissionsConfig>
parse_workspace_block(const json& block, std::string_view path, bool strict, std::vector<ConfigWarning>& warnings) {
  auto block_object = require_object(block, path);
  if (!block_object) {
    return std::unexpected(std::move(block_object.error()));
  }

  auto out = WorkspacePermissionsConfig{};
  if (const auto it = block.find("extra_read_roots"); it != block.end()) {
    auto parsed = string_array(*it, child_path(path, "extra_read_roots"));
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    out.extra_read_roots = std::move(*parsed);
  }
  if (const auto it = block.find("extra_write_roots"); it != block.end()) {
    auto parsed = string_array(*it, child_path(path, "extra_write_roots"));
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    out.extra_write_roots = std::move(*parsed);
  }

  static constexpr auto kKnownKeys = std::array<std::string_view, 2>{"extra_read_roots", "extra_write_roots"};
  for (const auto& [key, _] : block.items()) {
    if (std::ranges::contains(kKnownKeys, key)) {
      continue;
    }
    const auto field_path = child_path(path, key);
    if (strict) {
      return std::unexpected(config_error("unknown workspace field", field_path));
    }
    warnings.push_back(ConfigWarning{
        .path = field_path,
        .message = "unknown workspace field",
    });
  }

  return out;
}

[[nodiscard]] Result<PermissionsConfig>
parse_permissions_block(const json& block, std::string_view path, bool strict, std::vector<ConfigWarning>& warnings) {
  auto block_object = require_object(block, path);
  if (!block_object) {
    return std::unexpected(std::move(block_object.error()));
  }

  auto out = PermissionsConfig{};
  for (const auto& [key, value] : block.items()) {
    const auto key_path = child_path(path, key);
    if (key == "workspace") {
      auto parsed = parse_workspace_block(value, key_path, strict, warnings);
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      out.workspace = std::move(*parsed);
      continue;
    }
    const auto known_verdict = core::parse_enum<PermissionVerdict>(key);
    if (!known_verdict) {
      if (strict) {
        return std::unexpected(config_error("unknown verdict key", key_path));
      }
      warnings.push_back(ConfigWarning{
          .path = key_path,
          .message = "unknown verdict key",
      });
      continue;
    }
    if (!value.is_array()) {
      return std::unexpected(config_error("expected array", key_path));
    }
    out.rules.reserve(out.rules.size() + value.size());
    auto index = std::size_t{0};
    for (const auto& item : value) {
      auto rule = parse_permission_rule(item, *known_verdict, element_path(key_path, index), strict, warnings);
      if (!rule) {
        return std::unexpected(std::move(rule.error()));
      }
      out.rules.push_back(std::move(*rule));
      ++index;
    }
  }
  return out;
}

[[nodiscard]] Result<PermissionsConfig>
parse_root_permissions(const json& root, bool strict, std::vector<ConfigWarning>& warnings) {
  const auto it = root.find("permissions");
  if (it == root.end()) {
    return PermissionsConfig{};
  }
  return parse_permissions_block(*it, "$.permissions", strict, warnings);
}

[[nodiscard]] Result<std::vector<AgentConfig>>
parse_agents(const json& root, bool strict, std::vector<ConfigWarning>& warnings) {
  auto agents = std::vector<AgentConfig>{};
  const auto it = root.find("agents");
  if (it == root.end()) {
    return agents;
  }
  auto object = require_object(*it, "$.agents");
  if (!object) {
    return std::unexpected(std::move(object.error()));
  }

  agents.reserve(it->size());
  for (const auto& [name, value] : it->items()) {
    const auto agent_path = child_path("$.agents", name);
    auto agent_object = require_object(value, agent_path);
    if (!agent_object) {
      return std::unexpected(std::move(agent_object.error()));
    }

    auto permissions = PermissionsConfig{};
    if (const auto perm_it = value.find("permissions"); perm_it != value.end()) {
      auto parsed = parse_permissions_block(*perm_it, child_path(agent_path, "permissions"), strict, warnings);
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      permissions = std::move(*parsed);
    }

    auto skills_enabled = std::optional<std::vector<std::string>>{};
    if (const auto skills_it = value.find("skills_enabled"); skills_it != value.end()) {
      auto parsed = parse_non_empty_string_array(*skills_it, child_path(agent_path, "skills_enabled"), "skill name");
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      skills_enabled = std::move(*parsed);
    }

    auto skills_deactivated = std::vector<std::string>{};
    if (const auto deactivated_it = value.find("skills_deactivated"); deactivated_it != value.end()) {
      auto parsed =
          parse_non_empty_string_array(*deactivated_it, child_path(agent_path, "skills_deactivated"), "skill name");
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      skills_deactivated = std::move(*parsed);
    }

    auto skills_expirations = std::vector<SkillExpirationConfig>{};
    if (const auto expirations_it = value.find("skills_expirations"); expirations_it != value.end()) {
      auto parsed = parse_skill_expirations(*expirations_it, child_path(agent_path, "skills_expirations"));
      if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
      }
      skills_expirations = std::move(*parsed);
    }

    auto prompt_overlay = std::string{};
    if (const auto overlay_it = value.find("prompt_overlay"); overlay_it != value.end()) {
      const auto overlay_path = child_path(agent_path, "prompt_overlay");
      if (!overlay_it->is_string()) {
        return std::unexpected(config_error("expected string", overlay_path));
      }
      prompt_overlay = overlay_it->get<std::string>();
    }

    for (const auto& [key, _] : value.items()) {
      if (is_recognized_agent_field(key)) {
        continue;
      }
      const auto field_path = child_path(agent_path, key);
      if (strict) {
        return std::unexpected(config_error("unknown agent field", field_path));
      }
      warnings.push_back(ConfigWarning{
          .path = field_path,
          .message = "unknown agent field",
      });
    }

    agents.push_back(AgentConfig{
        .name = name,
        .permissions = std::move(permissions),
        .prompt_overlay = std::move(prompt_overlay),
        .skills_enabled = std::move(skills_enabled),
        .skills_deactivated = std::move(skills_deactivated),
        .skills_expirations = std::move(skills_expirations),
    });
  }

  return agents;
}

}  // namespace

core::Result<Config> Config::parse(std::string_view contents, LoadOptions options) {
  try {
    auto root = json::parse(contents.begin(), contents.end());

    if (!root.is_object()) {
      return std::unexpected(config_error("expected root object", "$"));
    }

    auto strict = parse_strict_config(root);
    if (!strict) {
      return std::unexpected(std::move(strict.error()));
    }
    auto unknowns = collect_unknown_root_fields(root, options.strict_unknown_fields || *strict);
    if (!unknowns) {
      return std::unexpected(std::move(unknowns.error()));
    }
    auto env_result = substitute_env_recognized_roots(root);
    if (!env_result) {
      return std::unexpected(std::move(env_result.error()));
    }

    const auto strict_effective = options.strict_unknown_fields || *strict;
    auto warnings = std::move(*unknowns);

    auto runtime = parse_runtime(root);
    if (!runtime) {
      return std::unexpected(std::move(runtime.error()));
    }
    auto profiles = parse_profiles(root, strict_effective, warnings);
    if (!profiles) {
      return std::unexpected(std::move(profiles.error()));
    }
    auto routes = parse_routes(root, strict_effective, warnings);
    if (!routes) {
      return std::unexpected(std::move(routes.error()));
    }
    auto session = parse_session(root);
    if (!session) {
      return std::unexpected(std::move(session.error()));
    }
    auto web = parse_web(root);
    if (!web) {
      return std::unexpected(std::move(web.error()));
    }
    auto trace = parse_trace(root);
    if (!trace) {
      return std::unexpected(std::move(trace.error()));
    }
    auto hooks = parse_hooks(root, strict_effective, warnings);
    if (!hooks) {
      return std::unexpected(std::move(hooks.error()));
    }
    auto memory = parse_memory(root, strict_effective, warnings);
    if (!memory) {
      return std::unexpected(std::move(memory.error()));
    }
    auto permissions = parse_root_permissions(root, strict_effective, warnings);
    if (!permissions) {
      return std::unexpected(std::move(permissions.error()));
    }
    auto agents = parse_agents(root, strict_effective, warnings);
    if (!agents) {
      return std::unexpected(std::move(agents.error()));
    }

    auto config = Config{};
    config.strict_config_ = *strict;
    config.runtime_ = std::move(*runtime);
    config.profiles_ = std::move(*profiles);
    config.routes_ = std::move(*routes);
    config.session_ = std::move(*session);
    config.web_ = std::move(*web);
    config.trace_ = std::move(*trace);
    config.hooks_ = std::move(*hooks);
    config.memory_ = *memory;
    config.permissions_ = std::move(*permissions);
    config.agents_ = std::move(*agents);
    config.warnings_ = std::move(warnings);
    return config;
  } catch (const json::parse_error& e) {
    return std::unexpected(Error::config("failed to parse config JSON").with("detail", e.what()));
  } catch (const json::exception& e) {
    return std::unexpected(Error::config("failed to read config JSON").with("detail", e.what()));
  } catch (const std::exception& e) {
    return std::unexpected(Error::internal("config parser failed").with("detail", e.what()));
  }
}

core::Result<Config> Config::load_file(std::string_view path, LoadOptions options) {
  if (path.empty()) {
    return std::unexpected(Error::invalid_argument("config path is empty"));
  }

  auto path_text = std::string{path};
  try {
    const auto fs_path = std::filesystem::path{path_text};
    auto ec = std::error_code{};
    if (!std::filesystem::exists(fs_path, ec)) {
      if (ec) {
        return std::unexpected(
            Error::io("failed to check config file").with("path", path_text).with("detail", ec.message()));
      }
      return std::unexpected(Error::not_found("config file not found").with("path", path_text));
    }

    // Cap the file size before reading to bound memory under a malformed or
    // hostile config file. 16 MiB is far above any plausible hand-authored
    // config; the loader is single-shot at startup so a tighter bound is
    // appropriate. See docs/SECURITY.md "Sandbox Posture".
    const auto file_size = std::filesystem::file_size(fs_path, ec);
    if (ec) {
      return std::unexpected(
          Error::io("failed to stat config file").with("path", path_text).with("detail", ec.message()));
    }
    if (file_size > options.max_bytes) {
      return std::unexpected(Error::invalid_argument("config file exceeds max_bytes")
                                 .with("path", path_text)
                                 .with("size", std::to_string(file_size))
                                 .with("max_bytes", std::to_string(options.max_bytes)));
    }

    auto input = std::ifstream{fs_path, std::ios::binary};
    if (!input) {
      return std::unexpected(Error::io("failed to open config file").with("path", path_text));
    }

    auto contents = std::string{};
    contents.reserve(static_cast<std::size_t>(file_size));
    contents.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    if (input.bad()) {
      return std::unexpected(Error::io("failed to read config file").with("path", path_text));
    }

    return parse(contents, options);
  } catch (const std::filesystem::filesystem_error& e) {
    return std::unexpected(Error::io("failed to load config file").with("path", path_text).with("detail", e.what()));
  } catch (const std::exception& e) {
    return std::unexpected(Error::internal("config file load failed").with("path", path_text).with("detail", e.what()));
  }
}

}  // namespace orangutan::config

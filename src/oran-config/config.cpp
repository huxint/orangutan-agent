// src/oran-config/config.cpp — JSON-backed oran-config loader.

#include <oran/config/config.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/core/capability.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>

namespace orangutan::config {
namespace {

using json = ::nlohmann::ordered_json;
using ::orangutan::core::Error;
using ::orangutan::core::Result;

constexpr auto kRecognizedRootFields = std::array<std::string_view, 13>{
    "strict_config",
    "runtime",
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

constexpr auto kRecognizedAgentFields = std::array<std::string_view, 1>{
    "permissions",
};

[[nodiscard]] Error config_error(std::string message, std::string path) {
  return Error::config(std::move(message)).with("path", std::move(path));
}

[[nodiscard]] std::string child_path(std::string_view base, std::string_view child) {
  if (base.empty() || base == "$") {
    return std::string{"$."}.append(child);
  }
  return std::string{base}.append(".").append(child);
}

[[nodiscard]] std::string element_path(std::string_view base, std::size_t index) {
  return std::string{base}.append("[").append(std::to_string(index)).append("]");
}

[[nodiscard]] bool is_recognized_root(std::string_view name) {
  return std::ranges::find(kRecognizedRootFields, name) != kRecognizedRootFields.end();
}

[[nodiscard]] bool is_recognized_agent_field(std::string_view name) {
  return std::ranges::find(kRecognizedAgentFields, name) != kRecognizedAgentFields.end();
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

  if (const auto patterns = it->find("redaction_patterns"); patterns != it->end()) {
    auto parsed = string_array(*patterns, "$.runtime.redaction_patterns");
    if (!parsed) {
      return std::unexpected(std::move(parsed.error()));
    }
    runtime.redaction_patterns = std::move(*parsed);
  }

  return runtime;
}

[[nodiscard]] Result<std::vector<ProfileConfig>> parse_profiles(const json& root) {
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

    profiles.push_back(ProfileConfig{
        .name = name,
        .provider = std::move(*provider),
        .model = std::move(*model),
        .base_url = std::move(*base_url),
        .api_key_env = std::move(*api_key_env),
    });
  }

  return profiles;
}

[[nodiscard]] Result<std::vector<RouteConfig>> parse_routes(const json& root) {
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
    (void)value;
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

  static constexpr auto kKnownKeys = std::array<std::string_view, 2>{"tool_pattern", "capability"};
  for (const auto& [key, _] : value.items()) {
    if (std::ranges::find(kKnownKeys, key) != kKnownKeys.end()) {
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
  };
}

[[nodiscard]] Result<PermissionsConfig>
parse_permissions_block(const json& block, std::string_view path, bool strict, std::vector<ConfigWarning>& warnings) {
  auto block_object = require_object(block, path);
  if (!block_object) {
    return std::unexpected(std::move(block_object.error()));
  }

  auto out = PermissionsConfig{};
  for (const auto& [key, value] : block.items()) {
    const auto verdict_path = child_path(path, key);
    const auto known_verdict = core::parse_enum<PermissionVerdict>(key);
    if (!known_verdict) {
      if (strict) {
        return std::unexpected(config_error("unknown verdict key", verdict_path));
      }
      warnings.push_back(ConfigWarning{
          .path = verdict_path,
          .message = "unknown verdict key",
      });
      continue;
    }
    if (!value.is_array()) {
      return std::unexpected(config_error("expected array", verdict_path));
    }
    out.rules.reserve(out.rules.size() + value.size());
    auto index = std::size_t{0};
    for (const auto& item : value) {
      auto rule = parse_permission_rule(item, *known_verdict, element_path(verdict_path, index), strict, warnings);
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
    auto profiles = parse_profiles(root);
    if (!profiles) {
      return std::unexpected(std::move(profiles.error()));
    }
    auto routes = parse_routes(root);
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

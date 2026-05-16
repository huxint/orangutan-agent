// include/oran/config/config.hpp — typed JSON configuration loader.
//
// The public surface stays third-party-free: nlohmann::json is confined to the
// implementation file so config users do not inherit parser compile cost.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/capability.hpp>
#include <oran/core/result.hpp>

namespace orangutan::config {

struct ConfigWarning {
  std::string path;
  std::string message;
};

struct RuntimeConfig {
  std::int64_t workers{4};
  std::int64_t request_timeout_ms{600000};
  std::vector<std::string> redaction_patterns{};
};

struct ProfileConfig {
  std::string name;
  std::string provider;
  std::string model;
  std::string base_url;
  std::string api_key_env;
};

struct RouteConfig {
  std::string name;
  std::string primary_profile;
  std::vector<std::string> fallback_profiles{};
};

struct SessionConfig {
  bool auto_save{true};
  bool persistence{true};
};

struct WebConfig {
  bool enabled{false};
  std::string bind{"127.0.0.1"};
  std::int64_t port{8787};
};

/// Verdict spelling that appears in `config.permissions.{allow,deny,ask}`.
/// Mirrors `permission::Verdict` but stays inside `oran-config` because the
/// dependency direction (config below permission) forbids importing the
/// permission header here. The runtime materializer in `oran-permission`
/// maps `PermissionVerdict` to `permission::Verdict` one-to-one.
enum class PermissionVerdict : std::uint8_t {
  allow,
  deny,
  ask,
};

[[nodiscard]] std::string_view to_string_view(PermissionVerdict) noexcept;
[[nodiscard]] std::optional<PermissionVerdict> parse_permission_verdict(std::string_view) noexcept;

/// One config-side permission rule. `tool_pattern` is the `*`-glob the
/// `permission::RuleSet` matcher will consume. `capability`, when set, is
/// already resolved to a `core::Capability` value — unknown spellings fail
/// at load time so the materializer does not have to revalidate.
struct PermissionRuleConfig {
  PermissionVerdict verdict{PermissionVerdict::deny};
  std::string tool_pattern;
  std::optional<core::Capability> capability;

  friend bool operator==(const PermissionRuleConfig&, const PermissionRuleConfig&) = default;
};

/// Rules collected from one permissions block (the global `permissions`
/// root or a single agent's overlay). Rules appear in the JSON object's
/// iteration order so the operator's authoring intent survives the
/// materialize step (precedence is recovered by the runtime evaluator's
/// deny → allow → ask walk).
struct PermissionsConfig {
  std::vector<PermissionRuleConfig> rules;

  friend bool operator==(const PermissionsConfig&, const PermissionsConfig&) = default;
};

/// A single entry inside `agents.<name>`. The name field is the object key
/// (set by the parser, not authored). Future slices add provider/model
/// overrides, prompt templates, hook bindings, etc.; for now the typed
/// surface exposes only the per-agent permission overlay.
struct AgentConfig {
  std::string name;
  PermissionsConfig permissions;

  friend bool operator==(const AgentConfig&, const AgentConfig&) = default;
};

struct LoadOptions {
  bool strict_unknown_fields{false};
};

class Config {
public:
  Config() = default;

  [[nodiscard]] static core::Result<Config> parse(std::string_view contents, LoadOptions options = {});
  [[nodiscard]] static core::Result<Config> load_file(std::string_view path, LoadOptions options = {});

  [[nodiscard]] bool strict_config() const noexcept {
    return strict_config_;
  }
  [[nodiscard]] const RuntimeConfig& runtime() const noexcept {
    return runtime_;
  }
  [[nodiscard]] std::span<const ProfileConfig> profiles() const noexcept {
    return std::span<const ProfileConfig>{profiles_};
  }
  [[nodiscard]] std::span<const RouteConfig> routes() const noexcept {
    return std::span<const RouteConfig>{routes_};
  }
  [[nodiscard]] const SessionConfig& session() const noexcept {
    return session_;
  }
  [[nodiscard]] const WebConfig& web() const noexcept {
    return web_;
  }
  [[nodiscard]] const PermissionsConfig& permissions() const noexcept {
    return permissions_;
  }
  [[nodiscard]] std::span<const AgentConfig> agents() const noexcept {
    return std::span<const AgentConfig>{agents_};
  }
  [[nodiscard]] std::span<const ConfigWarning> warnings() const noexcept {
    return std::span<const ConfigWarning>{warnings_};
  }

private:
  bool strict_config_{false};
  RuntimeConfig runtime_{};
  std::vector<ProfileConfig> profiles_{};
  std::vector<RouteConfig> routes_{};
  SessionConfig session_{};
  WebConfig web_{};
  PermissionsConfig permissions_{};
  std::vector<AgentConfig> agents_{};
  std::vector<ConfigWarning> warnings_{};
};

}  // namespace orangutan::config

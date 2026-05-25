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

struct ToolOutputRuntimeConfig {
  std::int64_t max_text_bytes{256 * 1024};
  std::int64_t max_data_bytes{1024 * 1024};

  friend bool operator==(const ToolOutputRuntimeConfig&, const ToolOutputRuntimeConfig&) = default;
};

struct PromptActiveToolsConfig {
  bool use_defaults{true};
  std::vector<std::string> tool_names{};

  friend bool operator==(const PromptActiveToolsConfig&, const PromptActiveToolsConfig&) = default;
};

struct PromptRuntimeConfig {
  PromptActiveToolsConfig active_tools{};

  friend bool operator==(const PromptRuntimeConfig&, const PromptRuntimeConfig&) = default;
};

struct RuntimeConfig {
  std::int64_t workers{4};
  std::int64_t request_timeout_ms{600000};
  ToolOutputRuntimeConfig tool_output{};
  PromptRuntimeConfig prompt{};
  std::vector<std::string> redaction_patterns{};
};

/// One provider profile from `profiles.<name>`. `provider` remains the
/// operator/vendor label used by future adapter factories and secret lookup;
/// optional `protocol`, when set, is resolved by `oran-provider` as an exact
/// `provider::ProtocolKind` spelling so self-hosted gateways can select their
/// wire format without overloading the vendor label.
struct ProfileConfig {
  std::string name;
  std::string provider;
  std::optional<std::string> protocol;
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

struct TraceConfig {
  bool enabled{true};
  bool store_raw_bodies{false};
  std::int64_t retention_days{30};

  friend bool operator==(const TraceConfig&, const TraceConfig&) = default;
};

struct HooksConfig {
  std::int64_t timeout_ms{2000};

  friend bool operator==(const HooksConfig&, const HooksConfig&) = default;
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

/// One config-side permission rule. `tool_pattern` is the `*`-glob the
/// `permission::RuleSet` matcher will consume. `capability`, when set, is
/// already resolved to a `core::Capability` value — unknown spellings fail
/// at load time so the materializer does not have to revalidate.
/// `input_pattern`, when set, is a re2 source pattern string. The loader
/// pre-validates the regex (compiles + discards) so syntactically invalid
/// patterns fail at config load with the offending JSON path, mirroring the
/// criterion 4 "invalid patterns at load time are reported" guarantee in
/// `docs/product-specs/0008-permissions.md`. The materializer recompiles
/// the same pattern via `permission::InputPattern` when it assembles the
/// runtime `Rule`s. `replay_max` and `approval_ttl_seconds` carry the
/// per-rule approval-window policy (`docs/design-docs/permissions-and-hooks.md`
/// "Approval Signing": `replay_max=8`, `approval_ttl=3600s`). They are
/// optional at the config layer — when unset, the materializer keeps the
/// `permission::Rule` struct defaults, so an operator who omits the
/// fields gets the design-doc baseline.
struct PermissionRuleConfig {
  PermissionVerdict verdict{PermissionVerdict::deny};
  std::string tool_pattern;
  std::optional<core::Capability> capability{};
  std::optional<std::string> input_pattern{};
  std::optional<std::uint32_t> replay_max{};
  std::optional<std::int64_t> approval_ttl_seconds{};

  friend bool operator==(const PermissionRuleConfig&, const PermissionRuleConfig&) = default;
};

/// Workspace policy carried inside a permissions block. Extra read/write
/// roots widen the canonical workspace root after `tool::Workspace` builds
/// the resolver; the loader keeps the raw strings, leaving canonicalisation
/// and existence checks to `tool::Workspace::create` so config can be
/// parsed without touching the filesystem.
struct WorkspacePermissionsConfig {
  std::vector<std::string> extra_read_roots;
  std::vector<std::string> extra_write_roots;

  friend bool operator==(const WorkspacePermissionsConfig&, const WorkspacePermissionsConfig&) = default;
};

/// Rules collected from one permissions block (the global `permissions`
/// root or a single agent's overlay). Rules appear in the JSON object's
/// iteration order so the operator's authoring intent survives the
/// materialize step (precedence is recovered by the runtime evaluator's
/// deny → allow → ask walk).
struct PermissionsConfig {
  std::vector<PermissionRuleConfig> rules;
  WorkspacePermissionsConfig workspace;

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

  /// Hard cap on `load_file` size. The loader rejects files larger than this
  /// before any allocation, bounding memory cost under a malformed or hostile
  /// config. 16 MiB sits well above any plausible hand-authored configuration.
  std::uint64_t max_bytes{16ULL * 1024 * 1024};
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
  [[nodiscard]] const TraceConfig& trace() const noexcept {
    return trace_;
  }
  [[nodiscard]] const HooksConfig& hooks() const noexcept {
    return hooks_;
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
  TraceConfig trace_{};
  HooksConfig hooks_{};
  PermissionsConfig permissions_{};
  std::vector<AgentConfig> agents_{};
  std::vector<ConfigWarning> warnings_{};
};

}  // namespace orangutan::config

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
#include <oran/core/time.hpp>

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

/// Tool-scheduler knobs (spec 0012). Defaults mirror `agent::ToolSchedulerOptions`:
/// 4 concurrent tools, a 60 s per-call timeout, and a 5 min idle path-lock TTL.
/// Bootstrap converts these into the typed `ToolSchedulerOptions` it threads
/// into `AgentPromptRunner`.
struct ToolSchedulerRuntimeConfig {
  std::int64_t max_parallel_tools{4};
  std::int64_t per_call_timeout_ms{60000};
  std::int64_t idle_lock_ttl_ms{300000};

  friend bool operator==(const ToolSchedulerRuntimeConfig&, const ToolSchedulerRuntimeConfig&) = default;
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

/// Provider pricing in USD per one million tokens. Input and output prices are
/// separate because hosted models commonly price the two streams differently;
/// cache token prices are optional and default to the input token price when a
/// cost calculation consumes them.
struct ProviderPricingConfig {
  std::optional<double> input_per_million_usd{};
  std::optional<double> output_per_million_usd{};
  std::optional<double> cache_creation_per_million_usd{};
  std::optional<double> cache_read_per_million_usd{};

  [[nodiscard]] bool empty() const noexcept {
    return !input_per_million_usd.has_value() && !output_per_million_usd.has_value() &&
           !cache_creation_per_million_usd.has_value() && !cache_read_per_million_usd.has_value();
  }

  friend bool operator==(const ProviderPricingConfig&, const ProviderPricingConfig&) = default;
};

/// Byte bound for provider streaming responses. Mirrors the config loader's
/// own `max_bytes` cap on the stream consumption path: a provider stream may
/// hold at most this many wire bytes before the transfer is aborted with an
/// IO error, bounding memory regardless of wall-clock timeout.
struct StreamRuntimeConfig {
  std::int64_t max_bytes{16 * 1024 * 1024};

  friend bool operator==(const StreamRuntimeConfig&, const StreamRuntimeConfig&) = default;
};

struct RuntimeConfig {
  std::int64_t workers{4};
  std::int64_t request_timeout_ms{600000};
  ToolOutputRuntimeConfig tool_output{};
  ToolSchedulerRuntimeConfig tool_scheduler{};
  PromptRuntimeConfig prompt{};
  StreamRuntimeConfig stream{};
  std::vector<std::string> redaction_patterns{};
};

/// Optional per-profile prompt-cache policy. Mirrors the provider-layer
/// `PromptCacheOptions` shape so `oran-config` stays dependency-free.
struct PromptCacheConfig {
  bool enabled{true};
  std::int64_t min_prefix_bytes{0};

  friend bool operator==(const PromptCacheConfig&, const PromptCacheConfig&) = default;
};

/// One provider profile from `profiles.<name>`. `provider` remains the
/// operator/vendor label used by future adapter factories and secret lookup;
/// optional `protocol`, when set, is resolved by `oran-provider` as an exact
/// `provider::ProtocolKind` spelling so self-hosted gateways can select their
/// wire format without overloading the vendor label.
///
/// Optional `thinking_budget` and `cache` are per-profile policy applied by
/// `provider::execution::Runtime` to fallback attempts: a fallback profile
/// either carries its own budget/cache floor or has the primary's policy
/// stripped when the wire protocol cannot honor it.
struct ProfileConfig {
  std::string name;
  std::string provider;
  std::optional<std::string> protocol;
  std::string model;
  std::string base_url;
  std::string api_key_env;
  ProviderPricingConfig pricing;
  std::optional<std::uint32_t> thinking_budget{};
  std::optional<PromptCacheConfig> cache{};
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

struct DesktopConfig {
  bool enabled{false};
  std::string theme{"system"};
  bool reduce_motion{false};
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

enum class LongtermMemoryRecallQueryStrategy : std::uint8_t {
  prompt_text,
  last_user_message,
};

struct LongtermMemoryRecallConfig {
  bool enabled{false};
  std::int64_t limit{5};
  LongtermMemoryRecallQueryStrategy query_strategy{LongtermMemoryRecallQueryStrategy::prompt_text};
  std::vector<std::string> kinds{};

  friend bool operator==(const LongtermMemoryRecallConfig&, const LongtermMemoryRecallConfig&) = default;
};

/// Config-side hybrid-search policy. This is parser/runtime-contract prework:
/// bootstrap keeps using lexical recall until an embedding/vector backend owner
/// lands, but the operator-facing knobs are validated here first.
struct LongtermMemoryHybridSearchConfig {
  bool enabled{false};
  std::int64_t lexical_limit{10};
  std::int64_t vector_limit{10};
  std::int64_t result_limit{10};
  double lexical_weight{1.0};
  double vector_weight{1.0};

  friend bool operator==(const LongtermMemoryHybridSearchConfig&, const LongtermMemoryHybridSearchConfig&) = default;
};

struct LongtermMemoryRetentionConfig {
  std::int64_t forget_after_unused_days{180};
  double importance_floor{0.0};
  std::int64_t max_records_per_scope{10000};
  std::int64_t decay_check_interval_hours{24};

  friend bool operator==(const LongtermMemoryRetentionConfig&, const LongtermMemoryRetentionConfig&) = default;
};

struct LongtermMemoryConfig {
  LongtermMemoryRecallConfig recall{};
  LongtermMemoryHybridSearchConfig hybrid_search{};
  LongtermMemoryRetentionConfig retention{};

  friend bool operator==(const LongtermMemoryConfig&, const LongtermMemoryConfig&) = default;
};

struct MemoryConfig {
  LongtermMemoryConfig longterm{};

  friend bool operator==(const MemoryConfig&, const MemoryConfig&) = default;
};

struct AutomationCronJobConfig {
  std::string job_key;
  std::string agent_key{"automation"};
  std::string agent_prompt;
  std::string expression;
  core::Time first_fire_at{core::Time::epoch()};
  std::optional<core::Time> last_fired_at{};

  friend bool operator==(const AutomationCronJobConfig&, const AutomationCronJobConfig&) = default;
};

struct AutomationCronConfig {
  std::vector<AutomationCronJobConfig> jobs{};

  friend bool operator==(const AutomationCronConfig&, const AutomationCronConfig&) = default;
};

struct AutomationTriggeredJobConfig {
  std::string job_key;
  std::string trigger_key;
  std::string agent_key{"automation"};
  std::string agent_prompt;

  friend bool operator==(const AutomationTriggeredJobConfig&, const AutomationTriggeredJobConfig&) = default;
};

struct AutomationTriggeredConfig {
  std::vector<AutomationTriggeredJobConfig> jobs{};

  friend bool operator==(const AutomationTriggeredConfig&, const AutomationTriggeredConfig&) = default;
};

struct AutomationWebhookListenerConfig {
  bool enabled{false};
  std::string bind_host{"127.0.0.1"};
  std::uint16_t port{8787};
  std::string path_prefix{"/automation/webhooks/"};
  std::int64_t max_payload_bytes{256 * 1024};
  std::int64_t job_limit{100};

  friend bool operator==(const AutomationWebhookListenerConfig&, const AutomationWebhookListenerConfig&) = default;
};

struct AutomationWebhooksConfig {
  AutomationWebhookListenerConfig listener{};

  friend bool operator==(const AutomationWebhooksConfig&, const AutomationWebhooksConfig&) = default;
};

struct AutomationConfig {
  AutomationCronConfig cron{};
  AutomationTriggeredConfig triggered{};
  AutomationWebhooksConfig webhooks{};

  friend bool operator==(const AutomationConfig&, const AutomationConfig&) = default;
};

/// One config-authored channel adapter instance under `config.channels[]`.
/// `kind` selects the adapter implementation. `agent_key` names the agent that
/// answers messages arriving on this channel; bootstrap maps it through the
/// channel prompt-runner bridge. `inbound_capacity` bounds the adapter's
/// inbound queue. The `qq_*` fields are adapter-specific metadata consumed only
/// when `kind == "qq"` and the optional QQ adapter is compiled in; they store
/// environment-variable names and endpoint URLs, never secret values.
struct ChannelConfig {
  std::string id;
  std::string kind;
  std::string agent_key{"default"};
  std::size_t inbound_capacity{64};
  std::string qq_app_id_env;
  std::string qq_client_secret_env;
  std::string qq_token_url;
  std::string qq_api_base_url;
  std::string qq_gateway_url;

  friend bool operator==(const ChannelConfig&, const ChannelConfig&) = default;
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

/// A single skill-expiration entry inside `agents.<name>.skills_expirations`.
/// `expires_at` is the absolute UTC instant at or after which the skill's
/// section-4 active marker is dropped. The bootstrap runner supplies the
/// evaluation time when it resolves the policy, so the section-4 renderer never
/// reads a clock.
struct SkillExpirationConfig {
  std::string name;
  core::Time expires_at{};

  friend bool operator==(const SkillExpirationConfig&, const SkillExpirationConfig&) = default;
};

/// A single entry inside `agents.<name>`. The name field is the object key
/// (set by the parser, not authored). Future slices add provider/model
/// overrides, hook bindings, etc.; the typed surface now exposes per-agent
/// permissions, stable prompt overlay bytes, an optional skill allowlist, and
/// the explicit skill activation-policy inputs (deactivated names plus
/// expiration rows) the prompt runner feeds to `skill::resolve_active_skills`.
struct AgentConfig {
  std::string name;
  PermissionsConfig permissions;
  std::string prompt_overlay;
  std::optional<std::vector<std::string>> skills_enabled;
  std::vector<std::string> skills_deactivated;
  std::vector<SkillExpirationConfig> skills_expirations;

  friend bool operator==(const AgentConfig&, const AgentConfig&) = default;
};

struct LoadOptions {
  /// When enabled, unknown fields are rejected at the root plus typed nested
  /// config sections that already have a model (`profiles`, `pricing`,
  /// `routes`, `hooks`, `memory`, `permissions`, `workspace`, and `agents`).
  /// Loose mode preserves them as `ConfigWarning` rows and otherwise ignores
  /// them.
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
  [[nodiscard]] const DesktopConfig& desktop() const noexcept {
    return desktop_;
  }
  [[nodiscard]] const TraceConfig& trace() const noexcept {
    return trace_;
  }
  [[nodiscard]] const HooksConfig& hooks() const noexcept {
    return hooks_;
  }
  [[nodiscard]] const MemoryConfig& memory() const noexcept {
    return memory_;
  }
  [[nodiscard]] const AutomationConfig& automation() const noexcept {
    return automation_;
  }
  [[nodiscard]] std::span<const ChannelConfig> channels() const noexcept {
    return std::span<const ChannelConfig>{channels_};
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
  DesktopConfig desktop_{};
  TraceConfig trace_{};
  HooksConfig hooks_{};
  MemoryConfig memory_{};
  AutomationConfig automation_{};
  std::vector<ChannelConfig> channels_{};
  PermissionsConfig permissions_{};
  std::vector<AgentConfig> agents_{};
  std::vector<ConfigWarning> warnings_{};
};

}  // namespace orangutan::config

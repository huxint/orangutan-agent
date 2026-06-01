// include/oran/bootstrap/bootstrap.hpp — early runtime assembly entry points.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <oran/config.hpp>
#include <oran/core/result.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::bootstrap {

enum class ConfigSource : std::uint8_t {
  built_in_defaults,
  default_file,
  explicit_file,
};

[[nodiscard]] std::string_view to_string_view(ConfigSource source) noexcept;

struct BootstrapOptions {
  std::span<const std::string_view> args{};
  std::string workspace{"."};
};

struct LoadedConfig {
  config::Config value{};
  ConfigSource source{ConfigSource::built_in_defaults};
  std::string path{};
};

/// Selection inputs consumed by `--explain-rules` and by configured-route
/// prompt startup after parsing CLI args. `mode` defaults to
/// `permission::Mode::default_` (the diagnostic-friendly baseline the
/// original `--explain-rules` slice shipped); an empty `agent_name` means
/// "no per-agent overlay applied" for diagnostics and "default runtime
/// agent" for configured-route prompt runs.
struct ExplainRulesSelector {
  permission::Mode mode{permission::Mode::default_};
  std::string agent_name{};
};

[[nodiscard]] core::Result<LoadedConfig> load_config(BootstrapOptions options = {});
[[nodiscard]] core::Result<int> run(BootstrapOptions options = {});

/// Parse `--mode <name>` and `--agent <name>` (and their `=`-form variants)
/// out of `args`, leaving every other token alone. The returned selector
/// drives `materialize_rules` below and configured-route prompt-runner
/// selection. Invalid mode spellings and empty values surface as
/// `core::ErrorKind::invalid_argument` with the offending flag attached, so
/// tests can pin the rejection paths without invoking the full bootstrap.
[[nodiscard]] core::Result<ExplainRulesSelector> parse_explain_rules_selector(std::span<const std::string_view> args);

/// Build the materialized rule set described by `selector` against `cfg`.
/// When `selector.agent_name` is non-empty, the function looks up the
/// matching `config::AgentConfig` and applies its permissions overlay;
/// an unknown agent surfaces as `core::ErrorKind::not_found` with the
/// requested name attached. The empty-agent-name path mirrors the original
/// slice-10 behavior — no overlay applied.
[[nodiscard]] core::Result<permission::RuleSet> materialize_rules(const config::Config& cfg,
                                                                  const ExplainRulesSelector& selector);

}  // namespace orangutan::bootstrap

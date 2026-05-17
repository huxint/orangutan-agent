// tests/bootstrap/test_bootstrap.cpp — config-aware bootstrap coverage.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/bootstrap.hpp>
#include <oran/config/config.hpp>
#include <oran/core/error.hpp>
#include <oran/permission/rule_set.hpp>

namespace bootstrap = orangutan::bootstrap;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace permission = orangutan::permission;

namespace {

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  auto out = std::ofstream{path};
  out << contents;
}

bootstrap::BootstrapOptions options(std::vector<std::string_view>& args, const std::filesystem::path& workspace) {
  return bootstrap::BootstrapOptions{
      .args = std::span<const std::string_view>{args},
      .workspace = workspace.string(),
  };
}

constexpr auto kConfigText = R"json(
{
  "runtime": {
    "workers": 3
  },
  "profiles": {
    "default": {
      "provider": "anthropic",
      "model": "claude-3-5-sonnet-latest",
      "base_url": "https://api.anthropic.com",
      "api_key_env": "ANTHROPIC_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "default",
      "fallbacks": []
    }
  }
}
)json";

}  // namespace

TEST_CASE("load_config uses built-in defaults when default config is absent", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-no-config"};
  auto args = std::vector<std::string_view>{};

  auto loaded = bootstrap::load_config(options(args, temp.path()));

  REQUIRE(loaded.has_value());
  REQUIRE(loaded->source == bootstrap::ConfigSource::built_in_defaults);
  REQUIRE(loaded->path.ends_with(".orangutan/config.json"));
  REQUIRE(loaded->value.runtime().workers == 4);
  REQUIRE(loaded->value.profiles().empty());
}

TEST_CASE("load_config loads workspace default config when present", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-default-config"};
  const auto config_path = temp.path() / ".orangutan" / "config.json";
  write_file(config_path, kConfigText);
  auto args = std::vector<std::string_view>{};

  auto loaded = bootstrap::load_config(options(args, temp.path()));

  REQUIRE(loaded.has_value());
  REQUIRE(loaded->source == bootstrap::ConfigSource::default_file);
  REQUIRE(loaded->path == config_path.string());
  REQUIRE(loaded->value.runtime().workers == 3);
  REQUIRE(loaded->value.profiles().size() == 1);
  REQUIRE(loaded->value.routes().size() == 1);
}

TEST_CASE("load_config honors explicit config arguments", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-explicit-config"};
  const auto config_path = temp.path() / "config.json";
  write_file(config_path, kConfigText);

  SECTION("--config path") {
    auto config_arg = config_path.string();
    auto args = std::vector<std::string_view>{"--config", config_arg};
    auto loaded = bootstrap::load_config(options(args, temp.path()));

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->source == bootstrap::ConfigSource::explicit_file);
    REQUIRE(loaded->path == config_path.string());
    REQUIRE(loaded->value.runtime().workers == 3);
  }

  SECTION("--config=path") {
    auto config_arg = std::string{"--config="}.append(config_path.string());
    auto args = std::vector<std::string_view>{config_arg};
    auto loaded = bootstrap::load_config(options(args, temp.path()));

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->source == bootstrap::ConfigSource::explicit_file);
    REQUIRE(loaded->path == config_path.string());
  }

  SECTION("xmake run separator") {
    auto config_arg = config_path.string();
    auto args = std::vector<std::string_view>{"--", "--config", config_arg};
    auto loaded = bootstrap::load_config(options(args, temp.path()));

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->source == bootstrap::ConfigSource::explicit_file);
    REQUIRE(loaded->path == config_path.string());
  }
}

TEST_CASE("load_config ignores CLI arguments while resolving config", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-cli-args"};
  auto args = std::vector<std::string_view>{"--prompt", "hello"};

  auto loaded = bootstrap::load_config(options(args, temp.path()));

  REQUIRE(loaded.has_value());
  REQUIRE(loaded->source == bootstrap::ConfigSource::built_in_defaults);
  REQUIRE(loaded->value.runtime().workers == 4);
}

TEST_CASE("load_config rejects invalid bootstrap arguments", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-invalid-args"};

  SECTION("missing explicit path") {
    auto args = std::vector<std::string_view>{"--config"};
    auto loaded = bootstrap::load_config(options(args, temp.path()));

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind() == core::ErrorKind::invalid_argument);
  }

  SECTION("missing explicit file") {
    auto missing_path = (temp.path() / "missing.json").string();
    auto args = std::vector<std::string_view>{"--config", missing_path};
    auto loaded = bootstrap::load_config(options(args, temp.path()));

    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind() == core::ErrorKind::not_found);
  }
}

TEST_CASE("run handles help without loading config", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-help"};
  auto args = std::vector<std::string_view>{"--help"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE(result.has_value());
  REQUIRE(*result == 0);
}

TEST_CASE("run hands CLI arguments to oran-cli after config load", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-cli-handoff"};
  auto args = std::vector<std::string_view>{"--prompt", "hello"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE(result.has_value());
  REQUIRE(*result == 0);
}

TEST_CASE("run returns CLI argument errors after config load", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-cli-error"};
  auto args = std::vector<std::string_view>{"--unknown"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("run --audit-init applies the audit schema at the default workspace path", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-audit-init-default"};
  auto args = std::vector<std::string_view>{"--audit-init"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE(result.has_value());
  REQUIRE(*result == 0);
  const auto expected_db = temp.path() / ".orangutan" / "audit.db";
  REQUIRE(std::filesystem::exists(expected_db));

  // Idempotent on a second run.
  auto repeat = bootstrap::run(options(args, temp.path()));
  REQUIRE(repeat.has_value());
  REQUIRE(*repeat == 0);
}

TEST_CASE("run --audit-init honors an explicit audit-db path", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-audit-init-explicit"};
  const auto target = temp.path() / "nested" / "audit.db";
  auto target_str = target.string();

  SECTION("--audit-init <path>") {
    auto args = std::vector<std::string_view>{"--audit-init", target_str};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
    REQUIRE(std::filesystem::exists(target));
  }

  SECTION("--audit-init=<path>") {
    auto arg = std::string{"--audit-init="}.append(target_str);
    auto args = std::vector<std::string_view>{arg};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE(result.has_value());
    REQUIRE(*result == 0);
    REQUIRE(std::filesystem::exists(target));
  }
}

TEST_CASE("run --audit-init rejects empty explicit paths", "[unit][bootstrap]") {
  TempDir temp{"oran-bootstrap-audit-init-empty"};

  SECTION("--audit-init=") {
    auto args = std::vector<std::string_view>{"--audit-init="};
    auto result = bootstrap::run(options(args, temp.path()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  }
}

TEST_CASE("parse_explain_rules_selector defaults to default-mode, no agent", "[unit][bootstrap][explain_rules]") {
  auto args = std::vector<std::string_view>{};
  auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});

  REQUIRE(selector.has_value());
  REQUIRE(selector->mode == permission::Mode::default_);
  REQUIRE(selector->agent_name.empty());
}

TEST_CASE("parse_explain_rules_selector accepts every documented mode", "[unit][bootstrap][explain_rules]") {
  const auto cases = std::vector<std::pair<std::string_view, permission::Mode>>{
      {"strict", permission::Mode::strict},
      {"default", permission::Mode::default_},
      {"permissive", permission::Mode::permissive},
      {"sandboxed", permission::Mode::sandboxed},
  };

  for (const auto& [name, expected] : cases) {
    auto args = std::vector<std::string_view>{"--mode", name};
    auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});
    REQUIRE(selector.has_value());
    REQUIRE(selector->mode == expected);
  }
}

TEST_CASE("parse_explain_rules_selector accepts --mode=<name>", "[unit][bootstrap][explain_rules]") {
  auto args = std::vector<std::string_view>{"--mode=strict"};
  auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});

  REQUIRE(selector.has_value());
  REQUIRE(selector->mode == permission::Mode::strict);
}

TEST_CASE("parse_explain_rules_selector accepts --agent <name> and --agent=<name>",
          "[unit][bootstrap][explain_rules]") {
  SECTION("space-separated form") {
    auto args = std::vector<std::string_view>{"--agent", "researcher"};
    auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});
    REQUIRE(selector.has_value());
    REQUIRE(selector->agent_name == "researcher");
  }
  SECTION("eq-form") {
    auto args = std::vector<std::string_view>{"--agent=researcher"};
    auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});
    REQUIRE(selector.has_value());
    REQUIRE(selector->agent_name == "researcher");
  }
}

TEST_CASE("parse_explain_rules_selector rejects unknown mode spellings", "[unit][bootstrap][explain_rules]") {
  auto args = std::vector<std::string_view>{"--mode", "lax"};
  auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});

  REQUIRE_FALSE(selector.has_value());
  REQUIRE(selector.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("parse_explain_rules_selector rejects empty mode / agent values", "[unit][bootstrap][explain_rules]") {
  SECTION("missing --mode value") {
    auto args = std::vector<std::string_view>{"--mode"};
    auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});
    REQUIRE_FALSE(selector.has_value());
    REQUIRE(selector.error().kind() == core::ErrorKind::invalid_argument);
  }
  SECTION("empty --mode= value") {
    auto args = std::vector<std::string_view>{"--mode="};
    auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});
    REQUIRE_FALSE(selector.has_value());
    REQUIRE(selector.error().kind() == core::ErrorKind::invalid_argument);
  }
  SECTION("missing --agent value") {
    auto args = std::vector<std::string_view>{"--agent"};
    auto selector = bootstrap::parse_explain_rules_selector(std::span<const std::string_view>{args});
    REQUIRE_FALSE(selector.has_value());
    REQUIRE(selector.error().kind() == core::ErrorKind::invalid_argument);
  }
}

namespace {

constexpr auto kExplainConfigText = R"json(
{
  "permissions": {
    "allow": [
      {"tool_pattern": "file.read"}
    ],
    "deny": [
      {"tool_pattern": "*", "capability": "runtime_loader"}
    ]
  },
  "agents": {
    "researcher": {
      "permissions": {
        "allow": [
          {"tool_pattern": "*", "capability": "egress_http"}
        ]
      }
    }
  }
}
)json";

[[nodiscard]] config::Config load_explain_config() {
  auto parsed = config::Config::parse(kExplainConfigText);
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

[[nodiscard]] bool
has_rule_for_capability(const permission::RuleSet& rs, permission::Verdict verdict, core::Capability capability) {
  for (const auto& rule : rs.rules()) {
    if (rule.verdict == verdict && rule.capability == capability) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("materialize_rules without agent uses defaults + global only", "[unit][bootstrap][explain_rules]") {
  const auto cfg = load_explain_config();

  auto rs = bootstrap::materialize_rules(cfg, bootstrap::ExplainRulesSelector{.mode = permission::Mode::default_});

  REQUIRE(rs.has_value());
  // The researcher-only egress_http allow must NOT appear when no agent is selected.
  REQUIRE_FALSE(has_rule_for_capability(*rs, permission::Verdict::allow, core::Capability::egress_http));
}

TEST_CASE("materialize_rules applies a named agent overlay", "[unit][bootstrap][explain_rules]") {
  const auto cfg = load_explain_config();

  auto rs = bootstrap::materialize_rules(
      cfg,
      bootstrap::ExplainRulesSelector{.mode = permission::Mode::default_, .agent_name = "researcher"});

  REQUIRE(rs.has_value());
  REQUIRE(has_rule_for_capability(*rs, permission::Verdict::allow, core::Capability::egress_http));
}

TEST_CASE("materialize_rules respects mode-driven defaults", "[unit][bootstrap][explain_rules]") {
  const auto cfg = config::Config{};

  auto strict = bootstrap::materialize_rules(cfg, bootstrap::ExplainRulesSelector{.mode = permission::Mode::strict});
  auto def = bootstrap::materialize_rules(cfg, bootstrap::ExplainRulesSelector{.mode = permission::Mode::default_});
  auto permissive =
      bootstrap::materialize_rules(cfg, bootstrap::ExplainRulesSelector{.mode = permission::Mode::permissive});

  REQUIRE(strict.has_value());
  REQUIRE(def.has_value());
  REQUIRE(permissive.has_value());
  REQUIRE(strict->size() == 0);               // strict has no baseline rules
  REQUIRE(def->size() > permissive->size());  // default baseline is denser than permissive
}

TEST_CASE("materialize_rules surfaces unknown agent as not_found", "[unit][bootstrap][explain_rules]") {
  const auto cfg = load_explain_config();

  auto rs = bootstrap::materialize_rules(
      cfg,
      bootstrap::ExplainRulesSelector{.mode = permission::Mode::default_, .agent_name = "ghost"});

  REQUIRE_FALSE(rs.has_value());
  REQUIRE(rs.error().kind() == core::ErrorKind::not_found);
}

TEST_CASE("run --explain-rules accepts --mode and exits zero", "[unit][bootstrap][explain_rules]") {
  TempDir temp{"oran-bootstrap-explain-mode"};
  auto args = std::vector<std::string_view>{"--explain-rules", "--mode", "permissive"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE(result.has_value());
  REQUIRE(*result == 0);
}

TEST_CASE("run --explain-rules rejects unknown --mode", "[unit][bootstrap][explain_rules]") {
  TempDir temp{"oran-bootstrap-explain-bad-mode"};
  auto args = std::vector<std::string_view>{"--explain-rules", "--mode", "yolo"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("run --explain-rules rejects unknown --agent", "[unit][bootstrap][explain_rules]") {
  TempDir temp{"oran-bootstrap-explain-bad-agent"};
  const auto config_path = temp.path() / "config.json";
  write_file(config_path, kExplainConfigText);
  auto config_arg = config_path.string();
  auto args = std::vector<std::string_view>{"--config", config_arg, "--explain-rules", "--agent", "ghost"};

  auto result = bootstrap::run(options(args, temp.path()));

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::not_found);
}

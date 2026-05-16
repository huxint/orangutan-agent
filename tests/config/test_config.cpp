// tests/config/test_config.cpp — typed config loader coverage.

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/config.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>

namespace config = orangutan::config;
namespace core = orangutan::core;

namespace {

class ScopedEnv {
public:
  ScopedEnv(std::string name, std::string value) : name_(std::move(name)) {
    if (const auto* old = std::getenv(name_.c_str()); old != nullptr) {
      old_value_ = old;
    }
    setenv(name_.c_str(), value.c_str(), 1);
  }

  ~ScopedEnv() {
    if (old_value_) {
      setenv(name_.c_str(), old_value_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

class ScopedUnsetEnv {
public:
  explicit ScopedUnsetEnv(std::string name) : name_(std::move(name)) {
    if (const auto* old = std::getenv(name_.c_str()); old != nullptr) {
      old_value_ = old;
      unsetenv(name_.c_str());
    }
  }

  ~ScopedUnsetEnv() {
    if (old_value_) {
      setenv(name_.c_str(), old_value_->c_str(), 1);
    }
  }

  ScopedUnsetEnv(const ScopedUnsetEnv&) = delete;
  ScopedUnsetEnv& operator=(const ScopedUnsetEnv&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

constexpr auto kMinimalConfig = R"json(
{
  "runtime": {
    "workers": 2,
    "request_timeout_ms": 1500,
    "redaction_patterns": ["token=[^ ]+"]
  },
  "profiles": {
    "default": {
      "provider": "openai",
      "model": "gpt-5.5",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "default",
      "fallbacks": ["local"]
    }
  },
  "session": {
    "auto_save": false,
    "persistence": true
  },
  "web": {
    "enabled": true,
    "bind": "0.0.0.0",
    "port": 8787
  }
}
)json";

std::string example_config_path() {
  constexpr auto candidates = std::array<std::string_view, 5>{
      "config.example.json",
      "../config.example.json",
      "../../config.example.json",
      "../../../config.example.json",
      "../../../../config.example.json",
  };

  for (const auto candidate : candidates) {
    if (std::filesystem::exists(candidate)) {
      return std::string{candidate};
    }
  }
  return {};
}

}  // namespace

TEST_CASE("Config::parse returns typed config values", "[unit][config]") {
  auto result = config::Config::parse(kMinimalConfig);

  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->strict_config());
  REQUIRE(result->runtime().workers == 2);
  REQUIRE(result->runtime().request_timeout_ms == 1500);
  REQUIRE(result->runtime().redaction_patterns.size() == 1);

  REQUIRE(result->profiles().size() == 1);
  REQUIRE(result->profiles()[0].name == "default");
  REQUIRE(result->profiles()[0].provider == "openai");
  REQUIRE(result->profiles()[0].model == "gpt-5.5");
  REQUIRE(result->profiles()[0].base_url == "https://api.openai.com/v1");
  REQUIRE(result->profiles()[0].api_key_env == "OPENAI_API_KEY");

  REQUIRE(result->routes().size() == 1);
  REQUIRE(result->routes()[0].name == "default");
  REQUIRE(result->routes()[0].primary_profile == "default");
  REQUIRE(result->routes()[0].fallback_profiles == std::vector<std::string>{"local"});

  REQUIRE_FALSE(result->session().auto_save);
  REQUIRE(result->session().persistence);
  REQUIRE(result->web().enabled);
  REQUIRE(result->web().bind == "0.0.0.0");
  REQUIRE(result->web().port == 8787);
}

TEST_CASE("Config::parse recursively substitutes environment variables", "[unit][config]") {
  ScopedEnv model{"ORAN_CONFIG_TEST_MODEL", "model-from-env"};
  ScopedUnsetEnv base{"ORAN_CONFIG_TEST_BASE"};

  auto result = config::Config::parse(R"json(
{
  "runtime": {
    "redaction_patterns": ["${ORAN_CONFIG_TEST_MODEL}"]
  },
  "profiles": {
    "default": {
      "provider": "local",
      "model": "${ORAN_CONFIG_TEST_MODEL}",
      "base_url": "${ORAN_CONFIG_TEST_BASE:-http://127.0.0.1:8080}",
      "api_key_env": "LOCAL_API_KEY"
    }
  }
}
)json");

  REQUIRE(result.has_value());
  REQUIRE(result->runtime().redaction_patterns == std::vector<std::string>{"model-from-env"});
  REQUIRE(result->profiles()[0].model == "model-from-env");
  REQUIRE(result->profiles()[0].base_url == "http://127.0.0.1:8080");
}

TEST_CASE("Config::parse reports config errors without throwing", "[unit][config]") {
  SECTION("invalid JSON") {
    auto result = config::Config::parse("{");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("missing environment variable") {
    ScopedUnsetEnv missing{"ORAN_CONFIG_TEST_MISSING"};
    auto result = config::Config::parse(R"json(
{
  "profiles": {
    "default": {
      "provider": "local",
      "model": "${ORAN_CONFIG_TEST_MISSING}",
      "base_url": "http://127.0.0.1:8080",
      "api_key_env": "LOCAL_API_KEY"
    }
  }
}
)json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("typed value mismatch") {
    auto result = config::Config::parse(R"json({"web": {"port": 70000}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("integer out of range") {
    auto result = config::Config::parse(R"json({"runtime": {"workers": 18446744073709551615}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse warns or fails on unknown root fields", "[unit][config]") {
  ScopedUnsetEnv missing{"ORAN_CONFIG_TEST_MISSING"};

  auto loose = config::Config::parse(R"json({"future": true})json");
  REQUIRE(loose.has_value());
  REQUIRE(loose->warnings().size() == 1);
  REQUIRE(loose->warnings()[0].path == "$.future");

  auto loose_with_env = config::Config::parse(R"json({"future": "${ORAN_CONFIG_TEST_MISSING}"})json");
  REQUIRE(loose_with_env.has_value());
  REQUIRE(loose_with_env->warnings().size() == 1);
  REQUIRE(loose_with_env->warnings()[0].path == "$.future");

  auto strict_by_option =
      config::Config::parse(R"json({"future": true})json", config::LoadOptions{.strict_unknown_fields = true});
  REQUIRE_FALSE(strict_by_option.has_value());
  REQUIRE(strict_by_option.error().kind() == core::ErrorKind::config);

  auto strict_by_config = config::Config::parse(R"json({"strict_config": true, "future": true})json");
  REQUIRE_FALSE(strict_by_config.has_value());
  REQUIRE(strict_by_config.error().kind() == core::ErrorKind::config);
}

TEST_CASE("Config::load_file accepts the checked-in example config", "[unit][config]") {
  ScopedUnsetEnv default_model{"ORAN_DEFAULT_MODEL"};
  const auto path = example_config_path();
  REQUIRE_FALSE(path.empty());
  auto result = config::Config::load_file(path);

  REQUIRE(result.has_value());
  REQUIRE(result->profiles().size() == 1);
  REQUIRE(result->profiles()[0].name == "default");
  REQUIRE(result->profiles()[0].model == "claude-3-5-sonnet-latest");
  REQUIRE(result->routes().size() == 1);
  REQUIRE(result->web().port == 8787);

  // The example config now carries a non-empty permissions block + one
  // example agent overlay so the file documents the new schema.
  REQUIRE(result->permissions().rules.size() == 7);
  REQUIRE(result->agents().size() == 1);
  REQUIRE(result->agents()[0].name == "researcher");
  REQUIRE(result->agents()[0].permissions.rules.size() == 1);
}

TEST_CASE("PermissionVerdict round-trips through its stable spellings", "[unit][config]") {
  REQUIRE(core::enum_name(config::PermissionVerdict::allow) == "allow");
  REQUIRE(core::enum_name(config::PermissionVerdict::deny) == "deny");
  REQUIRE(core::enum_name(config::PermissionVerdict::ask) == "ask");

  using core::parse_enum;
  REQUIRE(parse_enum<config::PermissionVerdict>("allow") == config::PermissionVerdict::allow);
  REQUIRE(parse_enum<config::PermissionVerdict>("deny") == config::PermissionVerdict::deny);
  REQUIRE(parse_enum<config::PermissionVerdict>("ask") == config::PermissionVerdict::ask);
  REQUIRE_FALSE(parse_enum<config::PermissionVerdict>("approve").has_value());
  REQUIRE_FALSE(parse_enum<config::PermissionVerdict>("").has_value());
}

TEST_CASE("Config::parse extracts a populated permissions block", "[unit][config][permissions]") {
  auto result = config::Config::parse(R"json(
{
  "permissions": {
    "allow": [
      {"tool_pattern": "file.read"},
      {"tool_pattern": "*", "capability": "read_memory"}
    ],
    "deny": [
      {"tool_pattern": "*", "capability": "runtime_loader"}
    ],
    "ask": [
      {"tool_pattern": "file.write"},
      {"tool_pattern": "*", "capability": "spawn_subprocess"}
    ]
  }
}
)json");

  REQUIRE(result.has_value());
  const auto& perms = result->permissions();
  REQUIRE(perms.rules.size() == 5);

  REQUIRE(perms.rules[0].verdict == config::PermissionVerdict::allow);
  REQUIRE(perms.rules[0].tool_pattern == "file.read");
  REQUIRE_FALSE(perms.rules[0].capability.has_value());

  REQUIRE(perms.rules[1].verdict == config::PermissionVerdict::allow);
  REQUIRE(perms.rules[1].capability == core::Capability::read_memory);

  REQUIRE(perms.rules[2].verdict == config::PermissionVerdict::deny);
  REQUIRE(perms.rules[2].capability == core::Capability::runtime_loader);

  REQUIRE(perms.rules[3].verdict == config::PermissionVerdict::ask);
  REQUIRE(perms.rules[3].tool_pattern == "file.write");

  REQUIRE(perms.rules[4].verdict == config::PermissionVerdict::ask);
  REQUIRE(perms.rules[4].capability == core::Capability::spawn_subprocess);
}

TEST_CASE("Config::parse preserves authoring order across verdict keys", "[unit][config][permissions]") {
  // The JSON object iterates `ask` before `allow` here. The parser uses
  // object-iteration order so the operator's authoring intent survives.
  auto result = config::Config::parse(R"json(
{
  "permissions": {
    "ask": [{"tool_pattern": "first"}],
    "allow": [{"tool_pattern": "second"}],
    "deny": [{"tool_pattern": "third"}]
  }
}
)json");

  REQUIRE(result.has_value());
  const auto& rules = result->permissions().rules;
  REQUIRE(rules.size() == 3);
  REQUIRE(rules[0].verdict == config::PermissionVerdict::ask);
  REQUIRE(rules[0].tool_pattern == "first");
  REQUIRE(rules[1].verdict == config::PermissionVerdict::allow);
  REQUIRE(rules[1].tool_pattern == "second");
  REQUIRE(rules[2].verdict == config::PermissionVerdict::deny);
  REQUIRE(rules[2].tool_pattern == "third");
}

TEST_CASE("Config::parse extracts agents.<name>.permissions overlays", "[unit][config][permissions]") {
  auto result = config::Config::parse(R"json(
{
  "agents": {
    "researcher": {
      "permissions": {
        "allow": [{"tool_pattern": "*", "capability": "egress_http"}]
      }
    },
    "auditor": {
      "permissions": {
        "deny": [{"tool_pattern": "*", "capability": "write_file"}]
      }
    }
  }
}
)json");

  REQUIRE(result.has_value());
  REQUIRE(result->agents().size() == 2);
  REQUIRE(result->agents()[0].name == "researcher");
  REQUIRE(result->agents()[0].permissions.rules.size() == 1);
  REQUIRE(result->agents()[0].permissions.rules[0].capability == core::Capability::egress_http);
  REQUIRE(result->agents()[1].name == "auditor");
  REQUIRE(result->agents()[1].permissions.rules[0].verdict == config::PermissionVerdict::deny);
  REQUIRE(result->agents()[1].permissions.rules[0].capability == core::Capability::write_file);
}

TEST_CASE("Config::parse env-substitutes inside permission rules", "[unit][config][permissions]") {
  ScopedEnv pattern{"ORAN_CONFIG_TEST_PATTERN", "file.*"};

  auto result = config::Config::parse(R"json(
{
  "permissions": {
    "allow": [{"tool_pattern": "${ORAN_CONFIG_TEST_PATTERN}", "capability": "read_file"}]
  }
}
)json");

  REQUIRE(result.has_value());
  REQUIRE(result->permissions().rules.size() == 1);
  REQUIRE(result->permissions().rules[0].tool_pattern == "file.*");
  REQUIRE(result->permissions().rules[0].capability == core::Capability::read_file);
}

TEST_CASE("Config::parse rejects malformed permission rules", "[unit][config][permissions]") {
  SECTION("missing tool_pattern") {
    auto result = config::Config::parse(R"json({"permissions": {"allow": [{"capability": "read_file"}]}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("empty tool_pattern") {
    auto result = config::Config::parse(R"json({"permissions": {"allow": [{"tool_pattern": ""}]}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("unknown capability spelling") {
    auto result = config::Config::parse(
        R"json({"permissions": {"allow": [{"tool_pattern": "*", "capability": "transcend_reality"}]}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("non-object rule entry") {
    auto result = config::Config::parse(R"json({"permissions": {"allow": ["file.read"]}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("verdict array is not an array") {
    auto result = config::Config::parse(R"json({"permissions": {"allow": {"tool_pattern": "*"}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

TEST_CASE("Config::parse handles unknown verdict / rule / agent keys per mode", "[unit][config][permissions]") {
  SECTION("unknown verdict key warns in loose mode") {
    auto result = config::Config::parse(R"json({"permissions": {"approve": [{"tool_pattern": "*"}]}})json");
    REQUIRE(result.has_value());
    REQUIRE(result->permissions().rules.empty());
    REQUIRE(result->warnings().size() == 1);
    REQUIRE(result->warnings()[0].path == "$.permissions.approve");
  }

  SECTION("unknown verdict key fails under strict_config") {
    auto result = config::Config::parse(
        R"json({"strict_config": true, "permissions": {"approve": [{"tool_pattern": "*"}]}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }

  SECTION("unknown rule field warns in loose mode") {
    auto result =
        config::Config::parse(R"json({"permissions": {"allow": [{"tool_pattern": "*", "notes": "todo"}]}})json");
    REQUIRE(result.has_value());
    REQUIRE(result->permissions().rules.size() == 1);
    REQUIRE(result->warnings().size() == 1);
    REQUIRE(result->warnings()[0].path == "$.permissions.allow[0].notes");
  }

  SECTION("unknown agent field warns in loose mode") {
    auto result = config::Config::parse(R"json({"agents": {"a": {"model": "claude"}}})json");
    REQUIRE(result.has_value());
    REQUIRE(result->agents().size() == 1);
    REQUIRE(result->warnings().size() == 1);
    REQUIRE(result->warnings()[0].path == "$.agents.a.model");
  }

  SECTION("unknown agent field fails under strict_config") {
    auto result = config::Config::parse(R"json({"strict_config": true, "agents": {"a": {"model": "claude"}}})json");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::config);
  }
}

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
}

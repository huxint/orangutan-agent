// tests/provider/test_credentials.cpp - provider credential resolution coverage.

#include <oran/provider.hpp>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/error.hpp>

namespace core = orangutan::core;
namespace provider = orangutan::provider;

namespace {

class ScopedEnv {
public:
  ScopedEnv(std::string name, std::string value) : name_{std::move(name)} {
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
  explicit ScopedUnsetEnv(std::string name) : name_{std::move(name)} {
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

provider::ResolvedProfileTarget profile_target(std::string profile,
                                               std::string model,
                                               provider::ProtocolKind protocol,
                                               std::string provider_label,
                                               std::string base_url,
                                               std::string api_key_env) {
  return provider::ResolvedProfileTarget{
      .target =
          provider::ModelTarget{
              .profile = std::move(profile),
              .model = std::move(model),
              .protocol = protocol,
              .thinking_budget = std::nullopt,
              .cache = std::nullopt,
          },
      .provider = std::move(provider_label),
      .base_url = std::move(base_url),
      .api_key_env = std::move(api_key_env),
  };
}

provider::AdapterConstructionPlan adapter_plan() {
  auto resolution = provider::RouteProfileResolution{
      .primary = profile_target("anthropic-main",
                                "claude-sonnet",
                                provider::ProtocolKind::anthropic_messages,
                                "anthropic",
                                "https://api.anthropic.com",
                                "ORAN_PROVIDER_TEST_ANTHROPIC_KEY"),
      .fallbacks = {profile_target("openai-main",
                                   "gpt-main",
                                   provider::ProtocolKind::openai_responses,
                                   "openai",
                                   "https://api.openai.com/v1",
                                   "ORAN_PROVIDER_TEST_OPENAI_KEY")},
  };
  auto plan = provider::make_adapter_construction_plan(resolution);
  REQUIRE(plan.has_value());
  return std::move(*plan);
}

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace

TEST_CASE("adapter credential resolution reads API-key environment variables", "[unit][provider][credentials]") {
  ScopedEnv primary_key{"ORAN_PROVIDER_TEST_ANTHROPIC_KEY", "anthropic-secret"};
  ScopedEnv fallback_key{"ORAN_PROVIDER_TEST_OPENAI_KEY", "openai-secret"};
  const auto plan = adapter_plan();

  auto credentials = provider::resolve_adapter_credentials(plan);

  REQUIRE(credentials.has_value());
  REQUIRE(credentials->primary.target == plan.primary);
  REQUIRE(credentials->primary.api_key == "anthropic-secret");
  REQUIRE(credentials->fallbacks.size() == 1);
  REQUIRE(credentials->fallbacks[0].target == plan.fallbacks[0]);
  REQUIRE(credentials->fallbacks[0].api_key == "openai-secret");
  REQUIRE(credentials->route() == plan.route());
}

TEST_CASE("adapter credential resolution rejects missing API-key env vars", "[unit][provider][credentials]") {
  ScopedUnsetEnv missing{"ORAN_PROVIDER_TEST_ANTHROPIC_KEY"};
  auto plan = adapter_plan();

  auto credentials = provider::resolve_adapter_credentials(plan);

  REQUIRE_FALSE(credentials.has_value());
  REQUIRE(credentials.error().kind() == core::ErrorKind::auth);
  REQUIRE(context_value(credentials.error(), "role") == std::optional<std::string_view>{"primary"});
  REQUIRE(context_value(credentials.error(), "profile") == std::optional<std::string_view>{"anthropic-main"});
  REQUIRE(context_value(credentials.error(), "api_key_env") ==
          std::optional<std::string_view>{"ORAN_PROVIDER_TEST_ANTHROPIC_KEY"});
}

TEST_CASE("adapter credential resolution rejects empty API-key env vars", "[unit][provider][credentials]") {
  ScopedEnv primary_key{"ORAN_PROVIDER_TEST_ANTHROPIC_KEY", "anthropic-secret"};
  ScopedEnv empty_fallback_key{"ORAN_PROVIDER_TEST_OPENAI_KEY", ""};
  auto plan = adapter_plan();

  auto credentials = provider::resolve_adapter_credentials(plan);

  REQUIRE_FALSE(credentials.has_value());
  REQUIRE(credentials.error().kind() == core::ErrorKind::auth);
  REQUIRE(context_value(credentials.error(), "role") == std::optional<std::string_view>{"fallback"});
  REQUIRE(context_value(credentials.error(), "profile") == std::optional<std::string_view>{"openai-main"});
  REQUIRE(context_value(credentials.error(), "api_key_env") ==
          std::optional<std::string_view>{"ORAN_PROVIDER_TEST_OPENAI_KEY"});
}

TEST_CASE("adapter credential resolution rejects malformed credential metadata", "[unit][provider][credentials]") {
  auto plan = adapter_plan();
  plan.primary.profile.api_key_env.clear();

  auto credentials = provider::resolve_adapter_credentials(plan);

  REQUIRE_FALSE(credentials.has_value());
  REQUIRE(credentials.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(credentials.error(), "role") == std::optional<std::string_view>{"primary"});
  REQUIRE(context_value(credentials.error(), "profile") == std::optional<std::string_view>{"anthropic-main"});
  REQUIRE(context_value(credentials.error(), "field") == std::optional<std::string_view>{"api_key_env"});
}

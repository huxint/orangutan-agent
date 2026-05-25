// tests/provider/test_adapter_plan.cpp - offline adapter planning coverage.

#include <oran/provider.hpp>

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

provider::RouteProfileResolution route_resolution() {
  return provider::RouteProfileResolution{
      .primary = profile_target("anthropic-main",
                                "claude-sonnet",
                                provider::ProtocolKind::anthropic_messages,
                                "anthropic",
                                "https://api.anthropic.com",
                                "ANTHROPIC_API_KEY"),
      .fallbacks = {profile_target("openai-main",
                                   "gpt-main",
                                   provider::ProtocolKind::openai_responses,
                                   "openai",
                                   "https://api.openai.com/v1",
                                   "OPENAI_API_KEY")},
  };
}

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace

TEST_CASE("adapter construction plan classifies resolved route profiles", "[unit][provider][adapter]") {
  const auto resolution = route_resolution();

  auto plan = provider::make_adapter_construction_plan(resolution);

  REQUIRE(plan.has_value());
  REQUIRE(plan->primary.profile == resolution.primary);
  REQUIRE(plan->primary.adapter_name == "anthropic_messages");
  REQUIRE(plan->fallbacks.size() == 1);
  REQUIRE(plan->fallbacks[0].profile == resolution.fallbacks[0]);
  REQUIRE(plan->fallbacks[0].adapter_name == "openai_responses");
  REQUIRE(plan->route() == resolution.route());
}

TEST_CASE("adapter construction plan rejects unsupported endpoint URL schemes", "[unit][provider][adapter]") {
  auto resolution = route_resolution();
  resolution.primary.base_url = "ftp://api.example.invalid";

  auto plan = provider::make_adapter_construction_plan(resolution);

  REQUIRE_FALSE(plan.has_value());
  REQUIRE(plan.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(plan.error(), "role") == std::optional<std::string_view>{"primary"});
  REQUIRE(context_value(plan.error(), "profile") == std::optional<std::string_view>{"anthropic-main"});
  REQUIRE(context_value(plan.error(), "field") == std::optional<std::string_view>{"base_url"});
  REQUIRE(context_value(plan.error(), "base_url") == std::optional<std::string_view>{"ftp://api.example.invalid"});
}

TEST_CASE("adapter construction plan rejects missing endpoint metadata", "[unit][provider][adapter]") {
  auto resolution = route_resolution();
  resolution.fallbacks[0].api_key_env.clear();

  auto plan = provider::make_adapter_construction_plan(resolution);

  REQUIRE_FALSE(plan.has_value());
  REQUIRE(plan.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(plan.error(), "role") == std::optional<std::string_view>{"fallback"});
  REQUIRE(context_value(plan.error(), "profile") == std::optional<std::string_view>{"openai-main"});
  REQUIRE(context_value(plan.error(), "field") == std::optional<std::string_view>{"api_key_env"});
}

TEST_CASE("adapter construction plan rejects unknown protocol enum values", "[unit][provider][adapter]") {
  auto resolution = route_resolution();
  resolution.primary.target.protocol = static_cast<provider::ProtocolKind>(255);

  auto plan = provider::make_adapter_construction_plan(resolution);

  REQUIRE_FALSE(plan.has_value());
  REQUIRE(plan.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(plan.error(), "role") == std::optional<std::string_view>{"primary"});
  REQUIRE(context_value(plan.error(), "profile") == std::optional<std::string_view>{"anthropic-main"});
  REQUIRE(context_value(plan.error(), "protocol") == std::optional<std::string_view>{"unknown"});
}

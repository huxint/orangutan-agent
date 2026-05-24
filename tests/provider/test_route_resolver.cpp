// tests/provider/test_route_resolver.cpp — config profile/route resolution.

#include <oran/provider.hpp>

#include <algorithm>
#include <optional>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <oran/config.hpp>
#include <oran/core/error.hpp>

namespace config = orangutan::config;
namespace core = orangutan::core;
namespace provider = orangutan::provider;

namespace {

constexpr auto kRoutingConfig = R"json(
{
  "profiles": {
    "anthropic-main": {
      "provider": "anthropic",
      "model": "claude-sonnet",
      "base_url": "https://api.anthropic.com",
      "api_key_env": "ANTHROPIC_API_KEY"
    },
    "openai-main": {
      "provider": "openai",
      "model": "gpt-main",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY"
    },
    "responses-main": {
      "provider": "openai_responses",
      "model": "gpt-responses",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY"
    },
    "local-main": {
      "provider": "deepseek",
      "model": "deepseek-coder",
      "base_url": "http://127.0.0.1:11434/v1",
      "api_key_env": "LOCAL_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "anthropic-main",
      "fallbacks": ["openai-main", "local-main"]
    },
    "responses": {
      "primary": "responses-main",
      "fallbacks": []
    }
  }
}
)json";

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  const auto it = std::ranges::find_if(error.context(), [&](const auto& entry) { return entry.first == key; });
  if (it == error.context().end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace

TEST_CASE("resolve_route maps configured primary and fallback profiles", "[unit][provider][route]") {
  auto parsed = config::Config::parse(kRoutingConfig);
  REQUIRE(parsed.has_value());

  auto route = provider::resolve_route(*parsed, "default");

  REQUIRE(route.has_value());
  REQUIRE(route->primary.profile == "anthropic-main");
  REQUIRE(route->primary.model == "claude-sonnet");
  REQUIRE(route->primary.protocol == provider::ProtocolKind::anthropic_messages);
  REQUIRE_FALSE(route->primary.thinking_budget.has_value());
  REQUIRE_FALSE(route->primary.cache.has_value());

  REQUIRE(route->fallbacks.size() == 2);
  REQUIRE(route->fallbacks[0].profile == "openai-main");
  REQUIRE(route->fallbacks[0].model == "gpt-main");
  REQUIRE(route->fallbacks[0].protocol == provider::ProtocolKind::openai_chat_completions);
  REQUIRE(route->fallbacks[1].profile == "local-main");
  REQUIRE(route->fallbacks[1].model == "deepseek-coder");
  REQUIRE(route->fallbacks[1].protocol == provider::ProtocolKind::custom_openai_compatible);
}

TEST_CASE("resolve_route accepts exact protocol spellings in profile provider", "[unit][provider][route]") {
  auto parsed = config::Config::parse(kRoutingConfig);
  REQUIRE(parsed.has_value());

  auto route = provider::resolve_route(*parsed, "responses");

  REQUIRE(route.has_value());
  REQUIRE(route->primary.profile == "responses-main");
  REQUIRE(route->primary.protocol == provider::ProtocolKind::openai_responses);
  REQUIRE(route->fallbacks.empty());
}

TEST_CASE("resolve_route rejects missing route names", "[unit][provider][route]") {
  auto parsed = config::Config::parse(kRoutingConfig);
  REQUIRE(parsed.has_value());

  auto route = provider::resolve_route(*parsed, "missing");

  REQUIRE_FALSE(route.has_value());
  REQUIRE(route.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(route.error(), "route") == std::optional<std::string_view>{"missing"});
}

TEST_CASE("resolve_route rejects routes that reference unknown profiles", "[unit][provider][route]") {
  auto parsed = config::Config::parse(R"json(
{
  "profiles": {
    "primary": {
      "provider": "anthropic",
      "model": "claude",
      "base_url": "https://api.anthropic.com",
      "api_key_env": "ANTHROPIC_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "primary",
      "fallbacks": ["missing"]
    }
  }
}
)json");
  REQUIRE(parsed.has_value());

  auto route = provider::resolve_route(*parsed);

  REQUIRE_FALSE(route.has_value());
  REQUIRE(route.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(route.error(), "route") == std::optional<std::string_view>{"default"});
  REQUIRE(context_value(route.error(), "profile") == std::optional<std::string_view>{"missing"});
  REQUIRE(context_value(route.error(), "role") == std::optional<std::string_view>{"fallback"});
}

TEST_CASE("resolve_route rejects unknown provider spellings", "[unit][provider][route]") {
  auto parsed = config::Config::parse(R"json(
{
  "profiles": {
    "bad": {
      "provider": "telepathy",
      "model": "unknown",
      "base_url": "http://127.0.0.1:1",
      "api_key_env": "BAD_API_KEY"
    }
  },
  "routes": {
    "default": {
      "primary": "bad"
    }
  }
}
)json");
  REQUIRE(parsed.has_value());

  auto route = provider::resolve_route(*parsed);

  REQUIRE_FALSE(route.has_value());
  REQUIRE(route.error().kind() == core::ErrorKind::config);
  REQUIRE(context_value(route.error(), "profile") == std::optional<std::string_view>{"bad"});
  REQUIRE(context_value(route.error(), "role") == std::optional<std::string_view>{"primary"});
  REQUIRE(context_value(route.error(), "provider") == std::optional<std::string_view>{"telepathy"});
}

TEST_CASE("resolve_route rejects empty route names", "[unit][provider][route]") {
  auto parsed = config::Config::parse(kRoutingConfig);
  REQUIRE(parsed.has_value());

  auto route = provider::resolve_route(*parsed, "");

  REQUIRE_FALSE(route.has_value());
  REQUIRE(route.error().kind() == core::ErrorKind::invalid_argument);
}

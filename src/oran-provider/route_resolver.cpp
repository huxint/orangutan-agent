// src/oran-provider/route_resolver.cpp — resolve config routes to provider routes.

#include <oran/provider/route_resolver.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/config/config.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>

namespace orangutan::provider {
namespace {

using orangutan::core::Error;

constexpr auto kAnthropicAliases = std::array<std::string_view, 2>{"anthropic", "claude"};
constexpr auto kOpenAiChatAliases = std::array<std::string_view, 3>{"openai", "openai_chat", "openai_chat_completions"};
constexpr auto kOpenAiResponsesAliases = std::array<std::string_view, 2>{"openai_responses", "openai_response"};
constexpr auto kGeminiAliases = std::array<std::string_view, 3>{"gemini", "google_gemini", "gemini_generate_content"};
constexpr auto kOpenAiCompatibleAliases = std::array<std::string_view, 5>{"custom_openai_compatible",
                                                                          "openai_compatible",
                                                                          "deepseek",
                                                                          "deepseek_chat",
                                                                          "local"};

[[nodiscard]] Error config_error(std::string message) {
  return Error::config(std::move(message));
}

[[nodiscard]] bool matches_alias(std::span<const std::string_view> aliases, std::string_view value) noexcept {
  return std::ranges::contains(aliases, value);
}

[[nodiscard]] std::optional<ProtocolKind> protocol_for_provider(std::string_view provider) noexcept {
  if (auto parsed = core::parse_enum<ProtocolKind>(provider)) {
    return parsed;
  }
  if (matches_alias(kAnthropicAliases, provider)) {
    return ProtocolKind::anthropic_messages;
  }
  if (matches_alias(kOpenAiChatAliases, provider)) {
    return ProtocolKind::openai_chat_completions;
  }
  if (matches_alias(kOpenAiResponsesAliases, provider)) {
    return ProtocolKind::openai_responses;
  }
  if (matches_alias(kGeminiAliases, provider)) {
    return ProtocolKind::gemini_generate_content;
  }
  if (matches_alias(kOpenAiCompatibleAliases, provider)) {
    return ProtocolKind::custom_openai_compatible;
  }
  return std::nullopt;
}

[[nodiscard]] core::Result<ProtocolKind>
resolve_protocol(const config::ProfileConfig& profile, std::string_view route_name, std::string_view role) {
  if (profile.protocol.has_value()) {
    auto parsed = core::parse_enum<ProtocolKind>(*profile.protocol);
    if (!parsed) {
      return std::unexpected(config_error("unknown provider protocol")
                                 .with("route", std::string{route_name})
                                 .with("profile", profile.name)
                                 .with("role", std::string{role})
                                 .with("protocol", *profile.protocol));
    }
    return *parsed;
  }

  auto protocol = protocol_for_provider(profile.provider);
  if (!protocol) {
    return std::unexpected(config_error("unknown provider protocol")
                               .with("route", std::string{route_name})
                               .with("profile", profile.name)
                               .with("role", std::string{role})
                               .with("provider", profile.provider));
  }
  return *protocol;
}

[[nodiscard]] const config::RouteConfig* find_route(const config::Config& config, std::string_view route_name) {
  const auto routes = config.routes();
  const auto it = std::ranges::find(routes, route_name, &config::RouteConfig::name);
  return it == routes.end() ? nullptr : std::addressof(*it);
}

[[nodiscard]] const config::ProfileConfig* find_profile(const config::Config& config, std::string_view profile_name) {
  const auto profiles = config.profiles();
  const auto it = std::ranges::find(profiles, profile_name, &config::ProfileConfig::name);
  return it == profiles.end() ? nullptr : std::addressof(*it);
}

[[nodiscard]] core::Result<ModelTarget> target_for_profile(const config::Config& config,
                                                           std::string_view profile_name,
                                                           std::string_view route_name,
                                                           std::string_view role) {
  const auto* profile = find_profile(config, profile_name);
  if (profile == nullptr) {
    return std::unexpected(config_error("route references an unknown provider profile")
                               .with("route", std::string{route_name})
                               .with("profile", std::string{profile_name})
                               .with("role", std::string{role}));
  }

  auto protocol = resolve_protocol(*profile, route_name, role);
  if (!protocol) {
    return std::unexpected(std::move(protocol).error());
  }

  return ModelTarget{
      .profile = profile->name,
      .model = profile->model,
      .protocol = *protocol,
      .thinking_budget = std::nullopt,
      .cache = std::nullopt,
  };
}

}  // namespace

core::Result<Route> resolve_route(const config::Config& config, std::string_view route_name) {
  if (route_name.empty()) {
    return std::unexpected(Error::invalid_argument("route name must be non-empty"));
  }

  const auto* route = find_route(config, route_name);
  if (route == nullptr) {
    return std::unexpected(config_error("provider route not found").with("route", std::string{route_name}));
  }

  auto primary = target_for_profile(config, route->primary_profile, route->name, "primary");
  if (!primary) {
    return std::unexpected(std::move(primary).error());
  }

  auto fallbacks = std::vector<ModelTarget>{};
  fallbacks.reserve(route->fallback_profiles.size());
  for (const auto& fallback_profile : route->fallback_profiles) {
    auto fallback = target_for_profile(config, fallback_profile, route->name, "fallback");
    if (!fallback) {
      return std::unexpected(std::move(fallback).error());
    }
    fallbacks.push_back(std::move(*fallback));
  }

  return Route{
      .primary = std::move(*primary),
      .fallbacks = std::move(fallbacks),
  };
}

}  // namespace orangutan::provider

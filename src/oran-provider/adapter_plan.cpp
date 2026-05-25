// src/oran-provider/adapter_plan.cpp - offline provider adapter planning.

#include <oran/provider/adapter_plan.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>

namespace orangutan::provider {
namespace {

using orangutan::core::Error;

[[nodiscard]] Error config_error(std::string message) {
  return Error::config(std::move(message));
}

[[nodiscard]] std::string role_name(std::string_view role) {
  return std::string{role};
}

[[nodiscard]] Error
target_error(std::string message, const ResolvedProfileTarget& profile, std::string_view role, std::string_view field) {
  return config_error(std::move(message))
      .with("role", role_name(role))
      .with("profile", profile.target.profile)
      .with("field", std::string{field});
}

[[nodiscard]] core::Result<void> require_non_empty(std::string_view value,
                                                   const ResolvedProfileTarget& profile,
                                                   std::string_view role,
                                                   std::string_view field) {
  if (!value.empty()) {
    return {};
  }
  return std::unexpected(target_error("provider adapter target field must be non-empty", profile, role, field));
}

[[nodiscard]] bool has_supported_url_scheme(std::string_view value) noexcept {
  return value.starts_with("http://") || value.starts_with("https://");
}

[[nodiscard]] core::Result<AdapterConstructionTarget> plan_target(ResolvedProfileTarget profile,
                                                                  std::string_view role) {
  if (auto valid = require_non_empty(profile.target.profile, profile, role, "profile"); !valid) {
    return std::unexpected(std::move(valid).error());
  }
  if (auto valid = require_non_empty(profile.target.model, profile, role, "model"); !valid) {
    return std::unexpected(std::move(valid).error());
  }
  if (auto valid = require_non_empty(profile.provider, profile, role, "provider"); !valid) {
    return std::unexpected(std::move(valid).error());
  }
  if (auto valid = require_non_empty(profile.base_url, profile, role, "base_url"); !valid) {
    return std::unexpected(std::move(valid).error());
  }
  if (!has_supported_url_scheme(profile.base_url)) {
    return std::unexpected(target_error("provider adapter base_url must use http or https", profile, role, "base_url")
                               .with("base_url", profile.base_url));
  }
  if (auto valid = require_non_empty(profile.api_key_env, profile, role, "api_key_env"); !valid) {
    return std::unexpected(std::move(valid).error());
  }

  const auto adapter_name = core::enum_name(profile.target.protocol);
  if (adapter_name == "unknown") {
    return std::unexpected(config_error("unknown provider adapter protocol")
                               .with("role", role_name(role))
                               .with("profile", profile.target.profile)
                               .with("protocol", std::string{adapter_name}));
  }

  return AdapterConstructionTarget{
      .profile = std::move(profile),
      .adapter_name = std::string{adapter_name},
  };
}

}  // namespace

Route AdapterConstructionPlan::route() const {
  auto route_fallbacks = std::vector<ModelTarget>{};
  route_fallbacks.reserve(fallbacks.size());
  for (const auto& fallback : fallbacks) {
    route_fallbacks.push_back(fallback.profile.target);
  }

  return Route{
      .primary = primary.profile.target,
      .fallbacks = std::move(route_fallbacks),
  };
}

core::Result<AdapterConstructionPlan> make_adapter_construction_plan(const RouteProfileResolution& resolution) {
  auto primary = plan_target(resolution.primary, "primary");
  if (!primary) {
    return std::unexpected(std::move(primary).error());
  }

  auto fallbacks = std::vector<AdapterConstructionTarget>{};
  fallbacks.reserve(resolution.fallbacks.size());
  for (const auto& profile : resolution.fallbacks) {
    auto fallback = plan_target(profile, "fallback");
    if (!fallback) {
      return std::unexpected(std::move(fallback).error());
    }
    fallbacks.push_back(std::move(*fallback));
  }

  return AdapterConstructionPlan{
      .primary = std::move(*primary),
      .fallbacks = std::move(fallbacks),
  };
}

}  // namespace orangutan::provider

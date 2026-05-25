// src/oran-provider/credentials.cpp - provider API-key env resolution.

#include <oran/provider/credentials.hpp>

#include <cstdlib>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>

namespace orangutan::provider {
namespace {

using orangutan::core::Error;
using orangutan::core::ErrorKind;

[[nodiscard]] std::string role_name(std::string_view role) {
  return std::string{role};
}

[[nodiscard]] Error
credential_error(ErrorKind kind, std::string message, const AdapterConstructionTarget& target, std::string_view role) {
  return Error{kind, std::move(message)}
      .with("role", role_name(role))
      .with("profile", target.profile.target.profile)
      .with("api_key_env", target.profile.api_key_env);
}

[[nodiscard]] core::Result<AdapterCredentialTarget> resolve_target_credentials(AdapterConstructionTarget target,
                                                                               std::string_view role) {
  if (target.profile.api_key_env.empty()) {
    return std::unexpected(
        credential_error(ErrorKind::config, "provider adapter target api_key_env must be non-empty", target, role)
            .with("field", "api_key_env"));
  }

  const auto* value = std::getenv(target.profile.api_key_env.c_str());
  if (value == nullptr) {
    return std::unexpected(
        credential_error(ErrorKind::auth, "provider API-key environment variable is not set", target, role));
  }

  auto api_key = std::string{value};
  if (api_key.empty()) {
    return std::unexpected(
        credential_error(ErrorKind::auth, "provider API-key environment variable is empty", target, role));
  }

  return AdapterCredentialTarget{
      .target = std::move(target),
      .api_key = std::move(api_key),
  };
}

}  // namespace

Route AdapterCredentialBundle::route() const {
  auto route_fallbacks = std::vector<ModelTarget>{};
  route_fallbacks.reserve(fallbacks.size());
  for (const auto& fallback : fallbacks) {
    route_fallbacks.push_back(fallback.target.profile.target);
  }

  return Route{
      .primary = primary.target.profile.target,
      .fallbacks = std::move(route_fallbacks),
  };
}

core::Result<AdapterCredentialBundle> resolve_adapter_credentials(const AdapterConstructionPlan& plan) {
  auto primary = resolve_target_credentials(plan.primary, "primary");
  if (!primary) {
    return std::unexpected(std::move(primary).error());
  }

  auto fallbacks = std::vector<AdapterCredentialTarget>{};
  fallbacks.reserve(plan.fallbacks.size());
  for (const auto& target : plan.fallbacks) {
    auto fallback = resolve_target_credentials(target, "fallback");
    if (!fallback) {
      return std::unexpected(std::move(fallback).error());
    }
    fallbacks.push_back(std::move(*fallback));
  }

  return AdapterCredentialBundle{
      .primary = std::move(*primary),
      .fallbacks = std::move(fallbacks),
  };
}

}  // namespace orangutan::provider

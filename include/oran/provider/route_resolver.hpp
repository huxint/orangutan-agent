#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <oran/core/result.hpp>
#include <oran/provider/system.hpp>

namespace orangutan::config {
class Config;
}  // namespace orangutan::config

namespace orangutan::provider {

/// Resolved model policy and endpoint metadata. `api_key_env` names a credential;
/// it contains no secret value.
struct ResolvedProfileTarget {
  ModelTarget target;
  std::string base_url;
  std::string api_key_env;

  friend bool operator==(const ResolvedProfileTarget&, const ResolvedProfileTarget&) = default;
};

struct RouteProfileResolution {
  ResolvedProfileTarget primary;
  std::vector<ResolvedProfileTarget> fallbacks;

  [[nodiscard]] Route route() const;

  friend bool operator==(const RouteProfileResolution&, const RouteProfileResolution&) = default;
};

/// Resolve profile names, protocol aliases and model policy without effects.
/// An explicit protocol takes precedence over the provider label. Transport
/// support and endpoint validation belong to `make_protocol_system`.
[[nodiscard]] core::Result<RouteProfileResolution> resolve_route_profiles(const config::Config& config,
                                                                          std::string_view route_name = "default");

}  // namespace orangutan::provider

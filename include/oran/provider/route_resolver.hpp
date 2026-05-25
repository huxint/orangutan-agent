// include/oran/provider/route_resolver.hpp — config route/profile resolver.
//
// `oran-config` owns the typed JSON profile/route data. This helper is the
// provider-side boundary that turns those names into the `provider::Route`
// value the agent loop and execution runtime already consume.

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

/// A configured profile after provider-side route resolution.
///
/// `target` is the loop/execution value. The remaining fields are the
/// endpoint metadata a protocol adapter factory needs later; `api_key_env` is
/// the configured environment-variable name, not a decrypted secret.
struct ResolvedProfileTarget {
  ModelTarget target;
  std::string provider;
  std::string base_url;
  std::string api_key_env;

  friend bool operator==(const ResolvedProfileTarget&, const ResolvedProfileTarget&) = default;
};

/// A route plus the profile endpoint metadata needed to construct adapters.
struct RouteProfileResolution {
  ResolvedProfileTarget primary;
  std::vector<ResolvedProfileTarget> fallbacks;

  [[nodiscard]] Route route() const;

  friend bool operator==(const RouteProfileResolution&, const RouteProfileResolution&) = default;
};

/// Resolve a configured route into adapter-factory-ready endpoint metadata.
///
/// This performs the same profile lookup and protocol parsing as
/// `resolve_route`, while preserving provider/base-url/API-key-env fields for
/// the future real adapter factory. It does not read credentials or construct a
/// provider backend.
[[nodiscard]] core::Result<RouteProfileResolution> resolve_route_profiles(const config::Config& config,
                                                                          std::string_view route_name = "default");

/// Resolve a configured route by name into a provider `Route`.
///
/// The current config surface carries provider/model/base-url/API-key metadata
/// plus an optional explicit profile protocol. Resolution fills
/// `{profile, model, protocol}` and leaves route-level cache / thinking options
/// unset until those typed config fields land. Explicit `profiles.*.protocol`
/// values are parsed as exact `ProtocolKind` spellings and take precedence over
/// provider aliases. Provider spelling aliases are intentionally resolved here,
/// keeping future protocol adapters dumb: they receive a `ProtocolKind`, not a
/// provider-name string.
[[nodiscard]] core::Result<Route> resolve_route(const config::Config& config, std::string_view route_name = "default");

}  // namespace orangutan::provider

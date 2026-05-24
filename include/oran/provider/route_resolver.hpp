// include/oran/provider/route_resolver.hpp — config route/profile resolver.
//
// `oran-config` owns the typed JSON profile/route data. This helper is the
// provider-side boundary that turns those names into the `provider::Route`
// value the agent loop and execution runtime already consume.

#pragma once

#include <string_view>

#include <oran/core/result.hpp>
#include <oran/provider/system.hpp>

namespace orangutan::config {
class Config;
}  // namespace orangutan::config

namespace orangutan::provider {

/// Resolve a configured route by name into a provider `Route`.
///
/// The current config surface carries only profile provider/model strings, so
/// resolution fills `{profile, model, protocol}` and leaves route-level cache /
/// thinking options unset until those typed config fields land. Provider
/// spelling aliases are intentionally resolved here, keeping future protocol
/// adapters dumb: they receive a `ProtocolKind`, not a provider-name string.
[[nodiscard]] core::Result<Route> resolve_route(const config::Config& config, std::string_view route_name = "default");

}  // namespace orangutan::provider

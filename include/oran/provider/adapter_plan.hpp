// include/oran/provider/adapter_plan.hpp - offline provider adapter planning.
//
// This is the offline validation seam before provider credential resolution:
// it consumes resolved profile metadata, validates the endpoint fields a
// factory will need, and records which protocol adapter family each target
// selects without reading secrets.

#pragma once

#include <string>
#include <vector>

#include <oran/core/result.hpp>
#include <oran/provider/route_resolver.hpp>
#include <oran/provider/system.hpp>

namespace orangutan::provider {

/// One resolved route target after adapter-factory preflight.
///
/// `adapter_name` is the reflection spelling of `profile.target.protocol`.
/// It is a non-secret identifier for logs/tests/future factory dispatch, not
/// a provider credential or transport handle.
struct AdapterConstructionTarget {
  ResolvedProfileTarget profile;
  std::string adapter_name;

  friend bool operator==(const AdapterConstructionTarget&, const AdapterConstructionTarget&) = default;
};

/// Primary + fallback adapter construction plan.
///
/// The plan is offline: it does not read API-key environment variables,
/// decrypt secrets, allocate an HTTP client, or construct a concrete backend.
struct AdapterConstructionPlan {
  AdapterConstructionTarget primary;
  std::vector<AdapterConstructionTarget> fallbacks;

  [[nodiscard]] Route route() const;

  friend bool operator==(const AdapterConstructionPlan&, const AdapterConstructionPlan&) = default;
};

/// Validate resolved route profile metadata and classify protocol adapters.
///
/// The preflight is intentionally shallow until `oran-http` and secret
/// accessors exist: provider/model/base-url/API-key-env fields must be
/// non-empty, `base_url` must use `http://` or `https://`, and the protocol
/// enum must be a known `ProtocolKind`.
[[nodiscard]] core::Result<AdapterConstructionPlan>
make_adapter_construction_plan(const RouteProfileResolution& resolution);

}  // namespace orangutan::provider

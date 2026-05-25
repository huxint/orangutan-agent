// include/oran/provider/credentials.hpp - provider API-key env resolution.
//
// This is the explicit secret-read step after offline adapter planning. It
// resolves the `api_key_env` names from an `AdapterConstructionPlan` into
// in-memory API-key strings for the future concrete adapter factory. Callers
// decide when to cross this boundary; ordinary bootstrap preflight does not.

#pragma once

#include <string>
#include <vector>

#include <oran/core/result.hpp>
#include <oran/provider/adapter_plan.hpp>
#include <oran/provider/system.hpp>

namespace orangutan::provider {

/// One adapter-plan target paired with its API-key value.
///
/// `api_key` is secret material read from the process environment. It must not
/// be logged, written to hook payloads, or echoed in errors.
struct AdapterCredentialTarget {
  AdapterConstructionTarget target;
  std::string api_key;
};

/// Primary + fallback credentials for an already-built adapter plan.
struct AdapterCredentialBundle {
  AdapterCredentialTarget primary;
  std::vector<AdapterCredentialTarget> fallbacks;

  [[nodiscard]] Route route() const;
};

/// Read the API-key environment variables named by an adapter plan.
///
/// Missing or empty environment variables return `ErrorKind::auth` with only
/// non-secret context (`role`, `profile`, and `api_key_env`). Malformed plans
/// whose `api_key_env` field is empty return `ErrorKind::config`.
[[nodiscard]] core::Result<AdapterCredentialBundle> resolve_adapter_credentials(const AdapterConstructionPlan& plan);

}  // namespace orangutan::provider

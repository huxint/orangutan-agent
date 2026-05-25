// include/oran/provider/adapter_factory.hpp - provider adapter factory seam.
//
// This is the construction boundary after credential resolution. It consumes
// in-memory adapter credentials and caller-registered protocol factories, then
// returns one provider::System that dispatches single-target calls to the
// backend built for the selected profile. It does not own HTTP transport code.

#pragma once

#include <memory>
#include <span>
#include <string_view>

#include <oran/core/result.hpp>
#include <oran/provider/credentials.hpp>
#include <oran/provider/system.hpp>

namespace orangutan::provider {

/// Factory for one protocol adapter family.
///
/// Implementations receive the already-resolved, in-memory API key for one
/// adapter target. The key is secret material and must not appear in logs,
/// hook payloads, or error context.
class ProtocolAdapterFactory {
public:
  ProtocolAdapterFactory() = default;
  virtual ~ProtocolAdapterFactory() = default;

  ProtocolAdapterFactory(const ProtocolAdapterFactory&) = delete;
  ProtocolAdapterFactory& operator=(const ProtocolAdapterFactory&) = delete;
  ProtocolAdapterFactory(ProtocolAdapterFactory&&) = delete;
  ProtocolAdapterFactory& operator=(ProtocolAdapterFactory&&) = delete;

  [[nodiscard]] virtual core::Result<std::unique_ptr<System>> create(AdapterCredentialTarget target) const = 0;
};

/// One adapter-family binding available to `make_adapter_system`.
///
/// `adapter_name` is the reflection spelling stored on
/// `AdapterConstructionTarget::adapter_name`, for example
/// `anthropic_messages` or `openai_responses`.
struct ProtocolAdapterFactoryBinding {
  std::string_view adapter_name;
  const ProtocolAdapterFactory* factory{nullptr};
};

/// Build a profile-routed provider system from resolved credentials.
///
/// The returned system expects the execution layer to pass a single selected
/// `Route::primary` target per call; it chooses the backend by
/// `route.primary.profile` and forwards a one-target route to that backend.
/// Retry and fallback remain owned by `provider::execution::Runtime`.
[[nodiscard]] core::Result<std::unique_ptr<System>>
make_adapter_system(AdapterCredentialBundle credentials, std::span<const ProtocolAdapterFactoryBinding> factories);

}  // namespace orangutan::provider

// src/oran-provider/adapter_factory.cpp - provider adapter factory seam.

#include <oran/provider/adapter_factory.hpp>

#include <algorithm>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/error.hpp>

namespace orangutan::provider {
namespace {

using orangutan::core::Error;

struct AdapterBackend {
  ModelTarget target;
  std::string adapter_name;
  std::unique_ptr<System> system;
};

[[nodiscard]] Error factory_error(std::string message, const AdapterConstructionTarget& target) {
  return Error::config(std::move(message))
      .with("profile", target.profile.target.profile)
      .with("adapter_name", target.adapter_name);
}

[[nodiscard]] core::Result<const ProtocolAdapterFactory*>
find_factory(std::string_view adapter_name, std::span<const ProtocolAdapterFactoryBinding> factories) {
  const auto match = std::ranges::find_if(factories, [&](const ProtocolAdapterFactoryBinding& binding) {
    return binding.adapter_name == adapter_name;
  });
  if (match == factories.end()) {
    return std::unexpected(
        Error::config("provider adapter factory not registered").with("adapter_name", std::string{adapter_name}));
  }
  return match->factory;
}

[[nodiscard]] core::Result<void> validate_factory_bindings(std::span<const ProtocolAdapterFactoryBinding> factories) {
  auto names = std::vector<std::string_view>{};
  names.reserve(factories.size());
  for (const auto& binding : factories) {
    if (binding.adapter_name.empty()) {
      return std::unexpected(Error::config("provider adapter factory binding name must be non-empty"));
    }
    if (binding.factory == nullptr) {
      return std::unexpected(Error::config("provider adapter factory binding must include a factory")
                                 .with("adapter_name", std::string{binding.adapter_name}));
    }
    if (std::ranges::contains(names, binding.adapter_name)) {
      return std::unexpected(Error::config("duplicate provider adapter factory binding")
                                 .with("adapter_name", std::string{binding.adapter_name}));
    }
    names.push_back(binding.adapter_name);
  }

  return {};
}

[[nodiscard]] core::Result<AdapterBackend> make_backend(AdapterCredentialTarget target,
                                                        std::span<const ProtocolAdapterFactoryBinding> factories) {
  const auto construction_target = target.target;
  if (construction_target.profile.target.profile.empty()) {
    return std::unexpected(factory_error("provider adapter target profile must be non-empty", construction_target)
                               .with("field", "profile"));
  }
  if (construction_target.adapter_name.empty()) {
    return std::unexpected(
        factory_error("provider adapter name must be non-empty", construction_target).with("field", "adapter_name"));
  }

  auto factory = find_factory(construction_target.adapter_name, factories);
  if (!factory) {
    return std::unexpected(std::move(factory).error().with("profile", construction_target.profile.target.profile));
  }

  auto system = (*factory)->create(std::move(target));
  if (!system) {
    return std::unexpected(std::move(system).error());
  }
  if (*system == nullptr) {
    return std::unexpected(factory_error("provider adapter factory returned null system", construction_target));
  }

  return AdapterBackend{
      .target = construction_target.profile.target,
      .adapter_name = construction_target.adapter_name,
      .system = std::move(*system),
  };
}

class ProfileRoutedSystem final : public System {
public:
  explicit ProfileRoutedSystem(std::vector<AdapterBackend> backends) : backends_{std::move(backends)} {}

  [[nodiscard]] async::Awaitable<core::Result<Response>>
  send(Request request, Route route, EventSink* sink = nullptr) const override {
    if (!route.fallbacks.empty()) {
      co_return std::unexpected(Error::config("provider adapter system expects a single selected route target")
                                    .with("profile", route.primary.profile)
                                    .with("fallbacks", std::to_string(route.fallbacks.size())));
    }

    const auto match = std::ranges::find_if(backends_, [&](const AdapterBackend& backend) {
      return backend.target.profile == route.primary.profile;
    });
    if (match == backends_.end()) {
      co_return std::unexpected(Error::config("provider adapter backend not available for route target")
                                    .with("profile", route.primary.profile)
                                    .with("model", route.primary.model));
    }

    co_return co_await match->system->send(std::move(request),
                                           Route{
                                               .primary = route.primary,
                                               .fallbacks = {},
                                           },
                                           sink);
  }

private:
  std::vector<AdapterBackend> backends_;
};

}  // namespace

core::Result<std::unique_ptr<System>> make_adapter_system(AdapterCredentialBundle credentials,
                                                          std::span<const ProtocolAdapterFactoryBinding> factories) {
  if (auto valid = validate_factory_bindings(factories); !valid) {
    return std::unexpected(std::move(valid).error());
  }

  auto backends = std::vector<AdapterBackend>{};
  backends.reserve(1 + credentials.fallbacks.size());

  auto primary = make_backend(std::move(credentials.primary), factories);
  if (!primary) {
    return std::unexpected(std::move(primary).error().with("role", "primary"));
  }
  backends.push_back(std::move(*primary));

  for (auto& fallback_target : credentials.fallbacks) {
    const auto& profile = fallback_target.target.profile.target.profile;
    if (!profile.empty() && std::ranges::any_of(backends, [&](const AdapterBackend& existing) {
          return existing.target.profile == profile;
        })) {
      return std::unexpected(Error::config("provider adapter route contains duplicate profiles")
                                 .with("profile", profile)
                                 .with("role", "fallback"));
    }

    auto fallback = make_backend(std::move(fallback_target), factories);
    if (!fallback) {
      return std::unexpected(std::move(fallback).error().with("role", "fallback"));
    }
    backends.push_back(std::move(*fallback));
  }

  return std::make_unique<ProfileRoutedSystem>(std::move(backends));
}

}  // namespace orangutan::provider

// src/oran-provider/protocol_transport.cpp - provider protocol transport seam.

#include <oran/provider/protocol_transport.hpp>

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/provider/protocol_response.hpp>

namespace orangutan::provider {
namespace {

using orangutan::core::Error;

[[nodiscard]] std::string protocol_name(ProtocolKind protocol) {
  return std::string{core::enum_name(protocol)};
}

[[nodiscard]] bool is_transport_protocol(ProtocolKind protocol) noexcept {
  return protocol == ProtocolKind::anthropic_messages || protocol == ProtocolKind::openai_responses;
}

[[nodiscard]] Error config_error(std::string message, const AdapterCredentialTarget& target) {
  return Error::config(std::move(message))
      .with("profile", target.target.profile.target.profile)
      .with("model", target.target.profile.target.model)
      .with("protocol", protocol_name(target.target.profile.target.protocol));
}

[[nodiscard]] std::string join_url(std::string_view base_url, std::string_view path) {
  auto url = std::string{base_url};
  if (url.ends_with('/') && path.starts_with('/')) {
    url.pop_back();
  } else if (!url.ends_with('/') && !path.starts_with('/')) {
    url.push_back('/');
  }
  url.append(path);
  return url;
}

[[nodiscard]] ProtocolHttpHeader header(std::string name, std::string value) {
  return ProtocolHttpHeader{.name = std::move(name), .value = std::move(value)};
}

[[nodiscard]] std::vector<ProtocolHttpHeader> protocol_headers(const AdapterCredentialTarget& target,
                                                               const ProtocolTransportAdapterFactoryOptions& options) {
  auto headers = std::vector<ProtocolHttpHeader>{
      header("content-type", "application/json"),
      header("accept", "application/json"),
  };

  switch (target.target.profile.target.protocol) {
    case ProtocolKind::anthropic_messages:
      headers.push_back(header("x-api-key", target.api_key));
      headers.push_back(header("anthropic-version", options.anthropic_version));
      break;
    case ProtocolKind::openai_responses:
      headers.push_back(header("authorization", "Bearer " + target.api_key));
      break;
    case ProtocolKind::openai_chat_completions:
    case ProtocolKind::gemini_generate_content:
    case ProtocolKind::custom_openai_compatible:
      break;
  }

  return headers;
}

[[nodiscard]] Error http_status_error(const ProtocolHttpResponse& response, const ModelTarget& target) {
  auto with_status = [&](Error error) {
    return std::move(error)
        .with("provider_profile", target.profile)
        .with("provider_model", target.model)
        .with("protocol", protocol_name(target.protocol))
        .with("http_status", std::to_string(response.status_code))
        .with("body_bytes", std::to_string(response.body_json.size()));
  };

  if (response.status_code == 401 || response.status_code == 403) {
    return with_status(Error{core::ErrorKind::auth, "provider authentication failed"});
  }
  if (response.status_code == 408) {
    return with_status(Error{core::ErrorKind::timeout, "provider request timed out"});
  }
  if (response.status_code == 429) {
    return with_status(Error::rate_limit("provider rate limited"));
  }
  if (response.status_code >= 500) {
    return with_status(Error::upstream("provider upstream error"));
  }
  if (response.status_code >= 400) {
    return with_status(Error::invalid_argument("provider rejected request"));
  }
  return with_status(Error::network("provider transport returned non-success status"));
}

[[nodiscard]] core::Result<void> validate_selected_route(const AdapterCredentialTarget& credentials,
                                                         const Route& route) {
  const auto& expected = credentials.target.profile.target;
  if (!route.fallbacks.empty()) {
    return std::unexpected(config_error("provider protocol adapter expects a single selected route target", credentials)
                               .with("fallbacks", std::to_string(route.fallbacks.size())));
  }
  if (route.primary.profile != expected.profile) {
    return std::unexpected(config_error("provider protocol adapter route profile mismatch", credentials)
                               .with("route_profile", route.primary.profile));
  }
  if (route.primary.model != expected.model) {
    return std::unexpected(config_error("provider protocol adapter route model mismatch", credentials)
                               .with("route_model", route.primary.model));
  }
  if (route.primary.protocol != expected.protocol) {
    return std::unexpected(config_error("provider protocol adapter route protocol mismatch", credentials)
                               .with("route_protocol", protocol_name(route.primary.protocol)));
  }
  return {};
}

class ProtocolTransportSystem final : public System {
public:
  ProtocolTransportSystem(ProtocolTransport& transport,
                          AdapterCredentialTarget credentials,
                          ProtocolTransportAdapterFactoryOptions options)
      : transport_{&transport}, credentials_{std::move(credentials)}, options_{std::move(options)} {}

  [[nodiscard]] async::Awaitable<core::Result<Response>>
  send(Request request, Route route, EventSink* sink = nullptr) const override {
    if (auto valid = validate_selected_route(credentials_, route); !valid) {
      co_return std::unexpected(std::move(valid).error());
    }

    request.stream = false;
    auto protocol = make_protocol_request(request, route.primary);
    if (!protocol) {
      co_return std::unexpected(std::move(protocol).error());
    }

    auto http_response = co_await transport_->send(ProtocolHttpRequest{
        .method = protocol->method,
        .url = join_url(credentials_.target.profile.base_url, protocol->path),
        .headers = protocol_headers(credentials_, options_),
        .body_json = std::move(protocol->body_json),
    });
    if (!http_response) {
      co_return std::unexpected(std::move(http_response)
                                    .error()
                                    .with("provider_profile", route.primary.profile)
                                    .with("provider_model", route.primary.model)
                                    .with("protocol", protocol_name(route.primary.protocol)));
    }

    if (http_response->status_code < 200 || http_response->status_code >= 300) {
      co_return std::unexpected(http_status_error(*http_response, route.primary));
    }

    auto decoded = decode_protocol_response(http_response->body_json, route.primary);
    if (!decoded) {
      co_return std::unexpected(std::move(decoded)
                                    .error()
                                    .with("provider_profile", route.primary.profile)
                                    .with("provider_model", route.primary.model));
    }
    if (sink != nullptr) {
      sink->on_done(decoded->stop_reason);
    }
    co_return decoded;
  }

private:
  ProtocolTransport* transport_;
  AdapterCredentialTarget credentials_;
  ProtocolTransportAdapterFactoryOptions options_;
};

}  // namespace

ProtocolTransportAdapterFactory::ProtocolTransportAdapterFactory(ProtocolTransport& transport,
                                                                 ProtocolKind protocol,
                                                                 ProtocolTransportAdapterFactoryOptions options)
    : transport_{&transport}, protocol_{protocol}, options_{std::move(options)} {}

core::Result<std::unique_ptr<System>> ProtocolTransportAdapterFactory::create(AdapterCredentialTarget target) const {
  if (!is_transport_protocol(protocol_)) {
    return std::unexpected(config_error("provider protocol transport factory protocol is not implemented", target)
                               .with("factory_protocol", protocol_name(protocol_)));
  }
  if (target.target.profile.target.protocol != protocol_) {
    return std::unexpected(config_error("provider protocol transport factory received the wrong protocol", target)
                               .with("factory_protocol", protocol_name(protocol_)));
  }
  if (target.target.profile.base_url.empty()) {
    return std::unexpected(config_error("provider protocol transport target base_url must be non-empty", target)
                               .with("field", "base_url"));
  }
  if (target.api_key.empty()) {
    return std::unexpected(
        config_error("provider protocol transport target api key must be non-empty", target).with("field", "api_key"));
  }
  return std::make_unique<ProtocolTransportSystem>(*transport_, std::move(target), options_);
}

std::vector<ProtocolAdapterFactoryBinding>
protocol_transport_factory_bindings(const ProtocolTransportAdapterFactory& anthropic,
                                    const ProtocolTransportAdapterFactory& openai) {
  return {
      ProtocolAdapterFactoryBinding{.adapter_name = "anthropic_messages", .factory = &anthropic},
      ProtocolAdapterFactoryBinding{.adapter_name = "openai_responses", .factory = &openai},
  };
}

}  // namespace orangutan::provider

// include/oran/provider/protocol_transport.hpp - provider protocol transport seam.
//
// This is the protocol-factory boundary before a concrete oran-http client
// exists. It composes the offline request/response mappers with an injected
// HTTP-shaped transport while keeping curl/asio implementation types out of
// provider public headers.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/provider/adapter_factory.hpp>
#include <oran/provider/protocol_request.hpp>
#include <oran/provider/system.hpp>

namespace orangutan::provider {

struct ProtocolHttpHeader {
  std::string name;
  std::string value;

  friend bool operator==(const ProtocolHttpHeader&, const ProtocolHttpHeader&) = default;
};

struct ProtocolHttpRequest {
  std::string method;
  std::string url;
  std::vector<ProtocolHttpHeader> headers;
  std::string body_json;

  friend bool operator==(const ProtocolHttpRequest&, const ProtocolHttpRequest&) = default;
};

struct ProtocolHttpResponse {
  std::uint16_t status_code{0};
  std::vector<ProtocolHttpHeader> headers;
  std::string body_json;

  friend bool operator==(const ProtocolHttpResponse&, const ProtocolHttpResponse&) = default;
};

/// Minimal async transport consumed by protocol adapters.
///
/// Implementations own actual HTTP, TLS, and SSE mechanics. Requests may carry
/// secret authorization headers; implementations must not log them.
class ProtocolTransport {
public:
  ProtocolTransport() = default;
  virtual ~ProtocolTransport() = default;

  ProtocolTransport(const ProtocolTransport&) = delete;
  ProtocolTransport& operator=(const ProtocolTransport&) = delete;
  ProtocolTransport(ProtocolTransport&&) = delete;
  ProtocolTransport& operator=(ProtocolTransport&&) = delete;

  [[nodiscard]] virtual async::Awaitable<core::Result<ProtocolHttpResponse>>
  send(ProtocolHttpRequest request) const = 0;
};

struct ProtocolTransportAdapterFactoryOptions {
  std::string anthropic_version{"2023-06-01"};
};

/// ProtocolAdapterFactory backed by an injected body transport.
///
/// The current implementation forces non-streaming vendor requests so the
/// transport can return one JSON response body. SSE streaming remains a later
/// concrete transport slice.
class ProtocolTransportAdapterFactory final : public ProtocolAdapterFactory {
public:
  ProtocolTransportAdapterFactory(ProtocolTransport& transport,
                                  ProtocolKind protocol,
                                  ProtocolTransportAdapterFactoryOptions options = {});

  [[nodiscard]] core::Result<std::unique_ptr<System>> create(AdapterCredentialTarget target) const override;

private:
  ProtocolTransport* transport_;
  ProtocolKind protocol_;
  ProtocolTransportAdapterFactoryOptions options_;
};

/// Convenience helper for the built-in Anthropic/OpenAI body-transport factories.
[[nodiscard]] std::vector<ProtocolAdapterFactoryBinding>
protocol_transport_factory_bindings(const ProtocolTransportAdapterFactory& anthropic,
                                    const ProtocolTransportAdapterFactory& openai);

}  // namespace orangutan::provider

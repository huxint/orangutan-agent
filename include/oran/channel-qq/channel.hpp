// include/oran/channel-qq/channel.hpp — QQ channel trait adapter.
//
// Normalizes QQ gateway message dispatches into the generic channel envelope
// and exposes the first `Channel::next_message()` boundary over the caller-owned
// gateway transport. Outbound sending is intentionally deferred to the next
// QQ-port slice; this adapter still implements the trait so receive-side wiring
// can be validated without bootstrap registration.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/channel/channel.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

#include <oran/channel-qq/gateway.hpp>

namespace orangutan::channel::qq {

class ApiClient;
class GatewayTransport;

struct QqDispatchNormalizationOptions {
  std::string channel_id{"qq-main"};
  orangutan::channel::Capabilities capabilities{
      .mentions = true,
      .reply_quoting = true,
      .max_text_bytes = 5'000,
  };
  core::Time received_at{};
};

/// Pure normalization seam for tests and future webhook reuse. Unsupported
/// non-message dispatches return `std::nullopt`; malformed supported message
/// payloads return `parsing` / `invalid_argument` with event context.
[[nodiscard]] core::Result<std::optional<orangutan::channel::InboundMessage>>
normalize_gateway_dispatch(GatewayDispatch dispatch, QqDispatchNormalizationOptions options = {});

struct QqChannelOptions {
  std::string id{"qq-main"};
  orangutan::channel::Capabilities capabilities{
      .mentions = true,
      .reply_quoting = true,
      .max_text_bytes = 5'000,
  };
};

class QqChannel final : public orangutan::channel::Channel {
public:
  QqChannel(GatewayTransport transport, ApiClient& api_client, QqChannelOptions options = {});
  ~QqChannel() override;

  QqChannel(const QqChannel&) = delete;
  QqChannel& operator=(const QqChannel&) = delete;
  QqChannel(QqChannel&&) noexcept;
  QqChannel& operator=(QqChannel&&) noexcept;

  [[nodiscard]] std::string_view id() const noexcept override;
  [[nodiscard]] std::string_view kind() const noexcept override;
  [[nodiscard]] orangutan::channel::Capabilities capabilities() const noexcept override;

  [[nodiscard]] async::Awaitable<core::Result<void>> start() override;
  [[nodiscard]] async::Awaitable<core::Result<void>> stop() override;
  [[nodiscard]] async::Awaitable<core::Result<orangutan::channel::InboundMessage>> next_message() override;
  [[nodiscard]] async::Awaitable<core::Result<orangutan::channel::DeliveryReceipt>>
  send(orangutan::channel::OutboundMessage message) override;

  [[nodiscard]] bool started() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::channel::qq

// include/oran/channel/mock.hpp — concrete in-process mock ingress adapter.

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>

#include <oran/channel/channel.hpp>

namespace orangutan::channel {

struct MockChannelOptions {
  std::string id{"mock-main"};
  std::string kind{"mock"};
  Capabilities capabilities{};
  std::size_t inbound_capacity{64};
};

/// The smallest concrete `Channel` adapter: an external producer pushes
/// inbound messages through `push_inbound(...)`, `next_message()` awaits the
/// bounded queue long-poll-style, and outbound sends are recorded with
/// deterministic receipts. It backs ingress/dispatch tests, benches, and
/// loopback wiring without opening any network listener.
class MockChannel final : public Channel {
public:
  explicit MockChannel(asio::any_io_executor executor, MockChannelOptions options = {});
  ~MockChannel() override;

  MockChannel(const MockChannel&) = delete;
  MockChannel& operator=(const MockChannel&) = delete;

  [[nodiscard]] std::string_view id() const noexcept override;
  [[nodiscard]] std::string_view kind() const noexcept override;
  [[nodiscard]] Capabilities capabilities() const noexcept override;

  [[nodiscard]] async::Awaitable<core::Result<void>> start() override;
  [[nodiscard]] async::Awaitable<core::Result<void>> stop() override;
  [[nodiscard]] async::Awaitable<core::Result<InboundMessage>> next_message() override;
  [[nodiscard]] async::Awaitable<core::Result<DeliveryReceipt>> send(OutboundMessage message) override;

  /// External-world entry: enqueue one inbound message without blocking.
  /// Fails with `mailbox_overflowed` when the bounded queue is full.
  [[nodiscard]] core::Result<void> push_inbound(InboundMessage message);

  [[nodiscard]] bool started() const noexcept;
  [[nodiscard]] std::size_t pending_inbound() const;
  [[nodiscard]] std::span<const OutboundMessage> sent_messages() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::channel

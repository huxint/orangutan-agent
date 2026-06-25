// src/oran-channel/mock.cpp — mock ingress adapter implementation.

#include <oran/channel/mock.hpp>

#include <cstddef>
#include <expected>
#include <format>
#include <utility>
#include <vector>

#include <oran/async/channel.hpp>
#include <oran/core/error.hpp>

namespace orangutan::channel {

namespace {

[[nodiscard]] MockChannelOptions normalize(MockChannelOptions options) noexcept {
  if (options.inbound_capacity == 0) {
    options.inbound_capacity = 1;
  }
  return options;
}

[[nodiscard]] core::Error not_started(std::string_view channel_id) {
  return core::Error{core::ErrorKind::conflict, "mock channel not started"}.with("channel_id", std::string{channel_id});
}

}  // namespace

struct MockChannel::Impl {
  Impl(asio::any_io_executor ex, MockChannelOptions opts)
      : executor{std::move(ex)}, options{normalize(std::move(opts))}, inbound{executor, options.inbound_capacity} {}

  asio::any_io_executor executor;
  MockChannelOptions options;
  async::Channel<InboundMessage> inbound;
  std::vector<OutboundMessage> sent;
  std::size_t send_count{0};
  bool started{false};
};

MockChannel::MockChannel(asio::any_io_executor executor, MockChannelOptions options)
    : impl_{std::make_unique<Impl>(std::move(executor), std::move(options))} {}

MockChannel::~MockChannel() = default;

std::string_view MockChannel::id() const noexcept {
  return impl_->options.id;
}

std::string_view MockChannel::kind() const noexcept {
  return impl_->options.kind;
}

Capabilities MockChannel::capabilities() const noexcept {
  return impl_->options.capabilities;
}

async::Awaitable<core::Result<void>> MockChannel::start() {
  if (impl_->inbound.closed()) {
    impl_->inbound = async::Channel<InboundMessage>{impl_->executor, impl_->options.inbound_capacity};
  }
  impl_->started = true;
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<void>> MockChannel::stop() {
  impl_->started = false;
  impl_->inbound.close();
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<InboundMessage>> MockChannel::next_message() {
  if (!impl_->started) {
    co_return std::unexpected(not_started(impl_->options.id));
  }
  co_return co_await impl_->inbound.receive();
}

async::Awaitable<core::Result<DeliveryReceipt>> MockChannel::send(OutboundMessage message) {
  if (!impl_->started) {
    co_return std::unexpected(not_started(impl_->options.id));
  }
  impl_->sent.push_back(std::move(message));
  ++impl_->send_count;
  co_return DeliveryReceipt{
      .message_id = std::format("{}-{}", impl_->options.id, impl_->send_count),
      .accepted_at = core::Time::epoch(),
  };
}

core::Result<void> MockChannel::push_inbound(InboundMessage message) {
  return impl_->inbound.try_send(std::move(message));
}

bool MockChannel::started() const noexcept {
  return impl_->started;
}

std::size_t MockChannel::pending_inbound() const {
  return impl_->inbound.size();
}

std::span<const OutboundMessage> MockChannel::sent_messages() const noexcept {
  return impl_->sent;
}

}  // namespace orangutan::channel

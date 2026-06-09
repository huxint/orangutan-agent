// src/oran-channel/manager.cpp — channel manager implementation.

#include <oran/channel/manager.hpp>

#include <algorithm>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <oran/async/channel.hpp>
#include <oran/core/error.hpp>

namespace orangutan::channel {

namespace {

[[nodiscard]] ChannelManagerOptions normalize(ChannelManagerOptions options) noexcept {
  if (options.inbound_capacity == 0) {
    options.inbound_capacity = 1;
  }
  return options;
}

[[nodiscard]] core::Error channel_not_found(std::string_view channel_id) {
  return core::Error::not_found("channel adapter not registered").with("channel_id", std::string{channel_id});
}

void normalize_inbound_message(const Channel& adapter, InboundMessage& message) {
  if (message.channel_id.empty()) {
    message.channel_id = std::string{adapter.id()};
  }
  if (message.origin.kind.empty()) {
    message.origin.kind = "channel";
  }
  if (message.origin.source.empty()) {
    message.origin.source = std::string{adapter.kind()};
  }
  message.caps = adapter.capabilities();
}

}  // namespace

struct ChannelManager::Impl {
  Impl(asio::any_io_executor ex, ChannelManagerOptions opts)
      : executor{std::move(ex)}, options{normalize(opts)}, inbound{executor, options.inbound_capacity} {}

  [[nodiscard]] Channel* find(std::string_view channel_id) noexcept {
    auto it = std::ranges::find_if(adapters, [channel_id](const auto& adapter) { return adapter->id() == channel_id; });
    return it == adapters.end() ? nullptr : it->get();
  }

  [[nodiscard]] const Channel* find(std::string_view channel_id) const noexcept {
    auto it = std::ranges::find_if(adapters, [channel_id](const auto& adapter) { return adapter->id() == channel_id; });
    return it == adapters.end() ? nullptr : it->get();
  }

  asio::any_io_executor executor;
  ChannelManagerOptions options;
  async::Channel<InboundMessage> inbound;
  std::vector<std::unique_ptr<Channel>> adapters;
};

ChannelManager::ChannelManager(asio::any_io_executor executor, ChannelManagerOptions options)
    : impl_{std::make_unique<Impl>(std::move(executor), options)} {}

ChannelManager::~ChannelManager() = default;

ChannelManager::ChannelManager(ChannelManager&&) noexcept = default;

ChannelManager& ChannelManager::operator=(ChannelManager&&) noexcept = default;

core::Result<void> ChannelManager::register_adapter(std::unique_ptr<Channel> adapter) {
  if (!adapter) {
    return std::unexpected(core::Error::invalid_argument("channel adapter is null"));
  }
  if (adapter->id().empty()) {
    return std::unexpected(core::Error::invalid_argument("channel adapter id is empty"));
  }
  if (impl_->find(adapter->id()) != nullptr) {
    return std::unexpected(
        core::Error{core::ErrorKind::conflict, "channel adapter already registered"}.with("channel_id",
                                                                                          std::string{adapter->id()}));
  }

  impl_->adapters.push_back(std::move(adapter));
  return {};
}

async::Awaitable<core::Result<void>> ChannelManager::start_all() {
  for (auto& adapter : impl_->adapters) {
    auto started = co_await adapter->start();
    if (!started) {
      auto error = std::move(started).error();
      error.with("channel_id", std::string{adapter->id()});
      co_return std::unexpected(std::move(error));
    }
  }
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<void>> ChannelManager::stop_all() {
  core::Result<void> first_error{};
  for (auto& adapter : impl_->adapters) {
    auto stopped = co_await adapter->stop();
    if (!stopped && first_error.has_value()) {
      auto error = std::move(stopped).error();
      error.with("channel_id", std::string{adapter->id()});
      first_error = std::unexpected(std::move(error));
    }
  }
  co_return first_error;
}

async::Awaitable<core::Result<void>> ChannelManager::receive_one(std::string_view channel_id) {
  auto* adapter = impl_->find(channel_id);
  if (adapter == nullptr) {
    co_return std::unexpected(channel_not_found(channel_id));
  }

  auto message = co_await adapter->next_message();
  if (!message) {
    co_return std::unexpected(std::move(message).error());
  }

  normalize_inbound_message(*adapter, *message);
  auto sent = co_await impl_->inbound.send(std::move(*message));
  if (!sent) {
    co_return std::unexpected(std::move(sent).error());
  }
  co_return core::Result<void>{};
}

async::Awaitable<core::Result<DeliveryReceipt>> ChannelManager::send(std::string_view channel_id,
                                                                     OutboundMessage message) {
  auto* adapter = impl_->find(channel_id);
  if (adapter == nullptr) {
    co_return std::unexpected(channel_not_found(channel_id));
  }
  co_return co_await adapter->send(std::move(message));
}

async::Channel<InboundMessage>& ChannelManager::inbound() {
  return impl_->inbound;
}

const async::Channel<InboundMessage>& ChannelManager::inbound() const {
  return impl_->inbound;
}

core::Result<Capabilities> ChannelManager::caps(std::string_view channel_id) const {
  auto* adapter = impl_->find(channel_id);
  if (adapter == nullptr) {
    return std::unexpected(channel_not_found(channel_id));
  }
  return adapter->capabilities();
}

bool ChannelManager::contains(std::string_view channel_id) const noexcept {
  return impl_->find(channel_id) != nullptr;
}

std::size_t ChannelManager::registered_count() const noexcept {
  return impl_->adapters.size();
}

}  // namespace orangutan::channel

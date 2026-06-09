// include/oran/channel/manager.hpp — caller-owned channel manager.

#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>

#include <oran/channel/channel.hpp>

namespace orangutan::async {
template <typename T>
class Channel;
}

namespace orangutan::channel {

struct ChannelManagerOptions {
  std::size_t inbound_capacity{64};
};

class ChannelManager {
public:
  explicit ChannelManager(asio::any_io_executor executor, ChannelManagerOptions options = {});
  ~ChannelManager();

  ChannelManager(const ChannelManager&) = delete;
  ChannelManager& operator=(const ChannelManager&) = delete;
  ChannelManager(ChannelManager&&) noexcept;
  ChannelManager& operator=(ChannelManager&&) noexcept;

  [[nodiscard]] core::Result<void> register_adapter(std::unique_ptr<Channel> adapter);

  [[nodiscard]] async::Awaitable<core::Result<void>> start_all();
  [[nodiscard]] async::Awaitable<core::Result<void>> stop_all();
  [[nodiscard]] async::Awaitable<core::Result<void>> receive_one(std::string_view channel_id);
  [[nodiscard]] async::Awaitable<core::Result<DeliveryReceipt>> send(std::string_view channel_id,
                                                                     OutboundMessage message);

  [[nodiscard]] async::Channel<InboundMessage>& inbound();
  [[nodiscard]] const async::Channel<InboundMessage>& inbound() const;
  [[nodiscard]] core::Result<Capabilities> caps(std::string_view channel_id) const;
  [[nodiscard]] bool contains(std::string_view channel_id) const noexcept;
  [[nodiscard]] std::size_t registered_count() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::channel

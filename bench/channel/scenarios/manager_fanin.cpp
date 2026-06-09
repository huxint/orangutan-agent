// bench/channel/scenarios/manager_fanin.cpp
//
// A-vs-B comparison: direct in-memory append vs. ChannelManager receive_one
// fan-in through the bounded async channel. This keeps the first channel
// abstraction cost visible before concrete adapters land.

#include <nanobench.h>

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/channel.hpp>
#include <oran/core/content.hpp>

namespace orangutan::bench {

namespace {

constexpr int kMessages = 16;

class BenchChannel final : public channel::Channel {
public:
  [[nodiscard]] std::string_view id() const noexcept override {
    return "bench";
  }

  [[nodiscard]] std::string_view kind() const noexcept override {
    return "mock";
  }

  [[nodiscard]] channel::Capabilities capabilities() const noexcept override {
    return {};
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> start() override {
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> stop() override {
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<channel::InboundMessage>> next_message() override {
    co_return channel::InboundMessage{
        .channel_id = {},
        .conversation_id = "bench-room",
        .user_id = "bench-user",
        .display_name = "Bench User",
        .content = {core::TextContent{.text = "hello"}},
        .replies_to = {},
        .received_at = core::Time::epoch(),
    };
  }

  [[nodiscard]] async::Awaitable<core::Result<channel::DeliveryReceipt>>
  send(channel::OutboundMessage /*message*/) override {
    co_return channel::DeliveryReceipt{.message_id = "bench-receipt", .accepted_at = core::Time::epoch()};
  }
};

[[gnu::noinline]] std::size_t run_direct_append() {
  std::vector<channel::InboundMessage> messages;
  messages.reserve(kMessages);
  for (int i = 0; i < kMessages; ++i) {
    messages.push_back(channel::InboundMessage{
        .channel_id = {},
        .conversation_id = "bench-room",
        .user_id = "bench-user",
        .display_name = "Bench User",
        .content = {core::TextContent{.text = "hello"}},
        .replies_to = {},
        .received_at = core::Time::epoch(),
    });
  }
  return messages.size();
}

[[gnu::noinline]] std::size_t run_manager_receive_one() {
  asio::io_context io;
  std::size_t received{};

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        channel::ChannelManager manager{io.get_executor(), channel::ChannelManagerOptions{.inbound_capacity = 32}};
        auto registered = manager.register_adapter(std::make_unique<BenchChannel>());
        if (!registered) {
          io.stop();
          co_return;
        }
        for (int i = 0; i < kMessages; ++i) {
          auto result = co_await manager.receive_one("bench");
          if (!result) {
            io.stop();
            co_return;
          }
        }
        while (true) {
          auto message = manager.inbound().try_receive();
          if (!message || !message->has_value()) {
            break;
          }
          ++received;
        }
        io.stop();
        co_return;
      },
      asio::detached);

  io.run();
  return received;
}

}  // namespace

void register_channel_manager_fanin(ankerl::nanobench::Bench& bench) {
  bench.run("channel.direct_append", [] {
    auto value = run_direct_append();
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("channel.manager_receive_one", [] {
    auto value = run_manager_receive_one();
    ankerl::nanobench::doNotOptimizeAway(value);
  });
}

}  // namespace orangutan::bench

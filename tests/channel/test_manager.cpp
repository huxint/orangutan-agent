// tests/channel/test_manager.cpp — channel manager foundation coverage.

#include <chrono>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/channel.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace channel = orangutan::channel;
namespace core = orangutan::core;
namespace test = orangutan::tests;

namespace {

class ScriptedChannel final : public channel::Channel {
public:
  ScriptedChannel(std::string id, std::string kind) : id_{std::move(id)}, kind_{std::move(kind)} {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] std::string_view kind() const noexcept override {
    return kind_;
  }

  [[nodiscard]] channel::Capabilities capabilities() const noexcept override {
    return capabilities_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> start() override {
    ++start_calls;
    if (start_error) {
      co_return std::unexpected(*start_error);
    }
    started = true;
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> stop() override {
    ++stop_calls;
    if (stop_error) {
      co_return std::unexpected(*stop_error);
    }
    stopped = true;
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<channel::InboundMessage>> next_message() override {
    ++next_calls;
    if (next_error) {
      co_return std::unexpected(*next_error);
    }
    if (messages.empty()) {
      co_return std::unexpected(core::Error::not_found("no scripted message"));
    }
    auto message = std::move(messages.front());
    messages.erase(messages.begin());
    co_return message;
  }

  [[nodiscard]] async::Awaitable<core::Result<channel::DeliveryReceipt>>
  send(channel::OutboundMessage message) override {
    ++send_calls;
    sent_messages.push_back(std::move(message));
    if (send_error) {
      co_return std::unexpected(*send_error);
    }
    co_return channel::DeliveryReceipt{.message_id = "sent-1", .accepted_at = core::Time::epoch()};
  }

  channel::Capabilities capabilities_{.rich_text = true, .reply_quoting = true, .max_text_bytes = 8192};
  std::vector<channel::InboundMessage> messages;
  std::vector<channel::OutboundMessage> sent_messages;
  std::optional<core::Error> start_error;
  std::optional<core::Error> stop_error;
  std::optional<core::Error> next_error;
  std::optional<core::Error> send_error;
  int start_calls{};
  int stop_calls{};
  int next_calls{};
  int send_calls{};
  bool started{false};
  bool stopped{false};

private:
  std::string id_;
  std::string kind_;
};

channel::InboundMessage inbound(std::string conversation_id = "room-1") {
  return channel::InboundMessage{
      .channel_id = {},
      .conversation_id = std::move(conversation_id),
      .user_id = "user-1",
      .display_name = "Operator",
      .content = {core::TextContent{.text = "hello"}},
      .replies_to = {},
      .received_at = core::Time::epoch(),
  };
}

std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  for (const auto& [entry_key, entry_value] : error.context()) {
    if (entry_key == key) {
      return std::string_view{entry_value};
    }
  }
  return std::nullopt;
}

}  // namespace

TEST_CASE("ChannelManager registers adapters and rejects duplicate ids", "[unit][channel][manager]") {
  asio::io_context io;
  channel::ChannelManager manager{io.get_executor()};

  auto registered = manager.register_adapter(std::make_unique<ScriptedChannel>("qq-main", "qq"));

  REQUIRE(registered.has_value());
  REQUIRE(manager.registered_count() == 1);
  REQUIRE(manager.contains("qq-main"));

  auto duplicate = manager.register_adapter(std::make_unique<ScriptedChannel>("qq-main", "qq"));
  REQUIRE_FALSE(duplicate.has_value());
  REQUIRE(duplicate.error().kind() == core::ErrorKind::conflict);
}

TEST_CASE("ChannelManager rejects null and unnamed adapters", "[unit][channel][manager]") {
  asio::io_context io;
  channel::ChannelManager manager{io.get_executor()};

  auto null_result = manager.register_adapter(nullptr);
  REQUIRE_FALSE(null_result.has_value());
  REQUIRE(null_result.error().kind() == core::ErrorKind::invalid_argument);

  auto unnamed = manager.register_adapter(std::make_unique<ScriptedChannel>("", "qq"));
  REQUIRE_FALSE(unnamed.has_value());
  REQUIRE(unnamed.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("ChannelManager starts and stops registered adapters in order", "[unit][channel][manager][async]") {
  auto* first = new ScriptedChannel{"qq-main", "qq"};
  auto* second = new ScriptedChannel{"webhook-main", "webhook"};

  test::run_async([first, second](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    REQUIRE(manager.register_adapter(std::unique_ptr<channel::Channel>{first}).has_value());
    REQUIRE(manager.register_adapter(std::unique_ptr<channel::Channel>{second}).has_value());

    auto started = co_await manager.start_all();
    REQUIRE(started.has_value());
    REQUIRE(first->started);
    REQUIRE(second->started);
    REQUIRE(first->start_calls == 1);
    REQUIRE(second->start_calls == 1);

    auto stopped = co_await manager.stop_all();
    REQUIRE(stopped.has_value());
    REQUIRE(first->stopped);
    REQUIRE(second->stopped);
    REQUIRE(first->stop_calls == 1);
    REQUIRE(second->stop_calls == 1);
  });
}

TEST_CASE("ChannelManager reports lifecycle errors with channel context", "[unit][channel][manager][async]") {
  auto* first = new ScriptedChannel{"qq-main", "qq"};
  auto* second = new ScriptedChannel{"webhook-main", "webhook"};
  first->stop_error = core::Error::internal("stop failed");
  second->stop_error = core::Error::timeout(std::chrono::milliseconds{25});

  test::run_async([first, second](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    REQUIRE(manager.register_adapter(std::unique_ptr<channel::Channel>{first}).has_value());
    REQUIRE(manager.register_adapter(std::unique_ptr<channel::Channel>{second}).has_value());

    auto stopped = co_await manager.stop_all();
    REQUIRE_FALSE(stopped.has_value());
    REQUIRE(stopped.error().kind() == core::ErrorKind::internal);
    REQUIRE(context_value(stopped.error(), "channel_id") == "qq-main");
    REQUIRE(first->stop_calls == 1);
    REQUIRE(second->stop_calls == 1);
  });
}

TEST_CASE("ChannelManager receives one normalized inbound message", "[unit][channel][manager][async]") {
  auto* adapter = new ScriptedChannel{"qq-main", "qq"};
  adapter->messages.push_back(inbound());

  test::run_async([adapter](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor(), channel::ChannelManagerOptions{.inbound_capacity = 2}};
    REQUIRE(manager.register_adapter(std::unique_ptr<channel::Channel>{adapter}).has_value());

    auto received = co_await manager.receive_one("qq-main");
    REQUIRE(received.has_value());

    auto queued = manager.inbound().try_receive();
    REQUIRE(queued.has_value());
    REQUIRE(queued->has_value());
    REQUIRE((*queued)->channel_id == "qq-main");
    REQUIRE((*queued)->conversation_id == "room-1");
    REQUIRE((*queued)->origin.kind == "channel");
    REQUIRE((*queued)->origin.source == "qq");
    REQUIRE((*queued)->caps.rich_text);
    REQUIRE((*queued)->caps.max_text_bytes == 8192);
  });
}

TEST_CASE("ChannelManager forwards adapter receive and send errors", "[unit][channel][manager][async]") {
  auto* adapter = new ScriptedChannel{"qq-main", "qq"};
  adapter->next_error = core::Error::upstream("adapter receive failed");
  adapter->send_error = core::Error::network("adapter send failed");

  test::run_async([adapter](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    REQUIRE(manager.register_adapter(std::unique_ptr<channel::Channel>{adapter}).has_value());

    auto received = co_await manager.receive_one("qq-main");
    REQUIRE_FALSE(received.has_value());
    REQUIRE(received.error().kind() == core::ErrorKind::upstream);
    auto queued = manager.inbound().try_receive();
    REQUIRE(queued.has_value());
    REQUIRE_FALSE(queued->has_value());

    auto sent = co_await manager.send("qq-main",
                                      channel::OutboundMessage{
                                          .conversation_id = "room-1",
                                          .content = {core::TextContent{.text = "response"}},
                                          .reactions = {},
                                      });
    REQUIRE_FALSE(sent.has_value());
    REQUIRE(sent.error().kind() == core::ErrorKind::network);
  });
}

TEST_CASE("ChannelManager routes outbound messages and capabilities by channel id", "[unit][channel][manager][async]") {
  auto* adapter = new ScriptedChannel{"webhook-main", "webhook"};

  test::run_async([adapter](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};
    REQUIRE(manager.register_adapter(std::unique_ptr<channel::Channel>{adapter}).has_value());

    auto caps = manager.caps("webhook-main");
    REQUIRE(caps.has_value());
    REQUIRE(caps->reply_quoting);

    auto receipt = co_await manager.send("webhook-main",
                                         channel::OutboundMessage{
                                             .conversation_id = "room-1",
                                             .content = {core::TextContent{.text = "response"}},
                                             .reactions = {},
                                         });

    REQUIRE(receipt.has_value());
    REQUIRE(receipt->message_id == "sent-1");
    REQUIRE(adapter->send_calls == 1);
    REQUIRE(adapter->sent_messages.size() == 1);
    REQUIRE(adapter->sent_messages.front().conversation_id == "room-1");
  });
}

TEST_CASE("ChannelManager reports missing channel ids", "[unit][channel][manager][async]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::ChannelManager manager{io.get_executor()};

    auto caps = manager.caps("missing");
    REQUIRE_FALSE(caps.has_value());
    REQUIRE(caps.error().kind() == core::ErrorKind::not_found);

    auto received = co_await manager.receive_one("missing");
    REQUIRE_FALSE(received.has_value());
    REQUIRE(received.error().kind() == core::ErrorKind::not_found);

    auto sent = co_await manager.send("missing",
                                      channel::OutboundMessage{
                                          .conversation_id = "room-1",
                                          .content = {},
                                          .reactions = {},
                                      });
    REQUIRE_FALSE(sent.has_value());
    REQUIRE(sent.error().kind() == core::ErrorKind::not_found);
  });
}

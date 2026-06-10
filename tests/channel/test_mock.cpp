// tests/channel/test_mock.cpp — concrete mock ingress adapter coverage.

#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>

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

channel::InboundMessage inbound(std::string text, std::string conversation_id = "room-1") {
  return channel::InboundMessage{
      .channel_id = {},
      .conversation_id = std::move(conversation_id),
      .user_id = "user-1",
      .display_name = "Operator",
      .content = {core::TextContent{.text = std::move(text)}},
      .replies_to = {},
      .received_at = core::Time::epoch(),
  };
}

}  // namespace

TEST_CASE("MockChannel reports identity, kind, and capabilities", "[unit][channel][mock]") {
  asio::io_context io;
  channel::MockChannel defaulted{io.get_executor()};
  REQUIRE(defaulted.id() == "mock-main");
  REQUIRE(defaulted.kind() == "mock");
  REQUIRE(defaulted.capabilities() == channel::Capabilities{});

  channel::MockChannel configured{io.get_executor(),
                                  channel::MockChannelOptions{
                                      .id = "mock-ingress",
                                      .kind = "mock",
                                      .capabilities = {.rich_text = true, .max_text_bytes = 1024},
                                      .inbound_capacity = 4,
                                  }};
  REQUIRE(configured.id() == "mock-ingress");
  REQUIRE(configured.capabilities().rich_text);
  REQUIRE(configured.capabilities().max_text_bytes == 1024);
}

TEST_CASE("MockChannel gates receive and send on lifecycle state", "[unit][channel][mock][async]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::MockChannel mock{io.get_executor()};
    REQUIRE_FALSE(mock.started());

    auto early_receive = co_await mock.next_message();
    REQUIRE_FALSE(early_receive.has_value());
    REQUIRE(early_receive.error().kind() == core::ErrorKind::conflict);

    auto early_send =
        co_await mock.send(channel::OutboundMessage{.conversation_id = "room-1", .content = {}, .reactions = {}});
    REQUIRE_FALSE(early_send.has_value());
    REQUIRE(early_send.error().kind() == core::ErrorKind::conflict);

    auto started = co_await mock.start();
    REQUIRE(started.has_value());
    REQUIRE(mock.started());
    REQUIRE(mock.push_inbound(inbound("hello")).has_value());
    auto received = co_await mock.next_message();
    REQUIRE(received.has_value());

    auto stopped = co_await mock.stop();
    REQUIRE(stopped.has_value());
    REQUIRE_FALSE(mock.started());
    auto late_send =
        co_await mock.send(channel::OutboundMessage{.conversation_id = "room-1", .content = {}, .reactions = {}});
    REQUIRE_FALSE(late_send.has_value());
    REQUIRE(late_send.error().kind() == core::ErrorKind::conflict);
  });
}

TEST_CASE("MockChannel delivers pushed messages in arrival order", "[unit][channel][mock][async]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::MockChannel mock{io.get_executor()};
    auto started = co_await mock.start();
    REQUIRE(started.has_value());

    REQUIRE(mock.push_inbound(inbound("first")).has_value());
    REQUIRE(mock.push_inbound(inbound("second")).has_value());
    REQUIRE(mock.pending_inbound() == 2);

    auto first = co_await mock.next_message();
    REQUIRE(first.has_value());
    REQUIRE(first->content.size() == 1);
    REQUIRE(core::text_view(first->content.front()) == "first");

    auto second = co_await mock.next_message();
    REQUIRE(second.has_value());
    REQUIRE(core::text_view(second->content.front()) == "second");
    REQUIRE(mock.pending_inbound() == 0);
  });
}

TEST_CASE("MockChannel next_message awaits a push that arrives later", "[unit][channel][mock][async]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::MockChannel mock{io.get_executor()};
    auto started = co_await mock.start();
    REQUIRE(started.has_value());

    auto pending = asio::co_spawn(io, mock.next_message(), asio::use_awaitable);

    // Let the receiver reach its queue await before the push arrives.
    auto slept = co_await async::sleep_for(io.get_executor(), std::chrono::milliseconds{1});
    REQUIRE(slept.has_value());
    REQUIRE(mock.push_inbound(inbound("late push")).has_value());

    auto received = co_await std::move(pending);
    REQUIRE(received.has_value());
    REQUIRE(core::text_view(received->content.front()) == "late push");
  });
}

TEST_CASE("MockChannel reports overflow when the inbound queue is full", "[unit][channel][mock]") {
  asio::io_context io;
  channel::MockChannel mock{io.get_executor(), channel::MockChannelOptions{.inbound_capacity = 1}};

  REQUIRE(mock.push_inbound(inbound("fits")).has_value());
  auto overflow = mock.push_inbound(inbound("dropped"));
  REQUIRE_FALSE(overflow.has_value());
  REQUIRE(overflow.error().kind() == core::ErrorKind::mailbox_overflowed);
}

TEST_CASE("MockChannel records outbound sends with deterministic receipts", "[unit][channel][mock][async]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    channel::MockChannel mock{io.get_executor()};
    auto started = co_await mock.start();
    REQUIRE(started.has_value());

    auto first = co_await mock.send(channel::OutboundMessage{
        .conversation_id = "room-1",
        .content = {core::TextContent{.text = "reply one"}},
        .reactions = {},
    });
    auto second = co_await mock.send(channel::OutboundMessage{
        .conversation_id = "room-2",
        .content = {core::TextContent{.text = "reply two"}},
        .reactions = {},
    });

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->message_id == "mock-main-1");
    REQUIRE(second->message_id == "mock-main-2");

    const auto sent = mock.sent_messages();
    REQUIRE(sent.size() == 2);
    REQUIRE(sent[0].conversation_id == "room-1");
    REQUIRE(sent[1].conversation_id == "room-2");
    REQUIRE(core::text_view(sent[0].content.front()) == "reply one");
  });
}

TEST_CASE("ChannelManager normalizes mock ingress messages", "[unit][channel][mock][manager][async]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto adapter = std::make_unique<channel::MockChannel>(io.get_executor());
    auto* mock_ptr = adapter.get();

    channel::ChannelManager manager{io.get_executor()};
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    auto started = co_await manager.start_all();
    REQUIRE(started.has_value());
    REQUIRE(mock_ptr->started());

    REQUIRE(mock_ptr->push_inbound(inbound("external hello")).has_value());
    auto received = co_await manager.receive_one("mock-main");
    REQUIRE(received.has_value());

    auto queued = manager.inbound().try_receive();
    REQUIRE(queued.has_value());
    REQUIRE(queued->has_value());
    REQUIRE((*queued)->channel_id == "mock-main");
    REQUIRE((*queued)->origin.kind == "channel");
    REQUIRE((*queued)->origin.source == "mock");
  });
}

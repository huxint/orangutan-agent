// tests/channel/test_dispatch.cpp — channel prompt dispatch seam coverage.

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

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

channel::InboundMessage inbound(std::vector<core::Content> content, std::string conversation_id = "room-1") {
  return channel::InboundMessage{
      .channel_id = "mock-main",
      .conversation_id = std::move(conversation_id),
      .user_id = "user-1",
      .display_name = "Operator",
      .content = std::move(content),
      .replies_to = {},
      .received_at = core::Time::epoch(),
      .origin = {.kind = "channel", .source = "mock"},
      .caps = {.rich_text = true},
  };
}

channel::ChannelPromptRunner echo_runner(std::vector<channel::ChannelPromptRunRequest>& seen) {
  return
      [&seen](
          channel::ChannelPromptRunRequest request) -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
        seen.push_back(request);
        co_return channel::ChannelPromptRunResult{.text = "echo: " + request.prompt};
      };
}

}  // namespace

TEST_CASE("make_prompt_run_request flattens text content into one prompt", "[unit][channel][dispatch]") {
  auto message = inbound({core::TextContent{.text = "first line"}, core::TextContent{.text = "second line"}});

  auto request = channel::make_prompt_run_request(message);

  REQUIRE(request.has_value());
  REQUIRE(request->prompt == "first line\nsecond line");
  REQUIRE(request->channel_id == "mock-main");
  REQUIRE(request->conversation_id == "room-1");
  REQUIRE(request->user_id == "user-1");
  REQUIRE(request->display_name == "Operator");
  REQUIRE(request->origin.source == "mock");
  REQUIRE(request->caps.rich_text);
}

TEST_CASE("make_prompt_run_request skips non-text blocks", "[unit][channel][dispatch]") {
  auto message =
      inbound({core::ThinkingContent{.thinking = "internal", .signature = {}}, core::TextContent{.text = "visible"}});

  auto request = channel::make_prompt_run_request(message);

  REQUIRE(request.has_value());
  REQUIRE(request->prompt == "visible");
}

TEST_CASE("make_prompt_run_request rejects messages without text", "[unit][channel][dispatch]") {
  auto empty = channel::make_prompt_run_request(inbound({}));
  REQUIRE_FALSE(empty.has_value());
  REQUIRE(empty.error().kind() == core::ErrorKind::invalid_argument);

  auto blank = channel::make_prompt_run_request(inbound({core::TextContent{.text = ""}}));
  REQUIRE_FALSE(blank.has_value());
  REQUIRE(blank.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("make_reply_message mirrors the inbound conversation", "[unit][channel][dispatch]") {
  auto message = inbound({core::TextContent{.text = "hello"}}, "room-7");

  auto reply = channel::make_reply_message(message, "answer");

  REQUIRE(reply.conversation_id == "room-7");
  REQUIRE(reply.content.size() == 1);
  REQUIRE(core::text_view(reply.content.front()) == "answer");
}

TEST_CASE("dispatch_one routes one queued message through the runner and replies", "[unit][channel][dispatch][async]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto adapter = std::make_unique<channel::MockChannel>(io.get_executor());
    auto* mock = adapter.get();
    channel::ChannelManager manager{io.get_executor()};
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    auto started = co_await manager.start_all();
    REQUIRE(started.has_value());

    REQUIRE(mock->push_inbound(inbound({core::TextContent{.text = "hello"}})).has_value());
    auto pumped = co_await manager.receive_one("mock-main");
    REQUIRE(pumped.has_value());

    std::vector<channel::ChannelPromptRunRequest> seen;
    auto receipt = co_await channel::dispatch_one(manager, echo_runner(seen));

    REQUIRE(receipt.has_value());
    REQUIRE(receipt->message_id == "mock-main-1");
    REQUIRE(seen.size() == 1);
    REQUIRE(seen.front().prompt == "hello");
    REQUIRE(seen.front().conversation_id == "room-1");

    const auto sent = mock->sent_messages();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent.front().conversation_id == "room-1");
    REQUIRE(core::text_view(sent.front().content.front()) == "echo: hello");
  });
}

TEST_CASE("dispatch_one propagates runner errors without sending", "[unit][channel][dispatch][async]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto adapter = std::make_unique<channel::MockChannel>(io.get_executor());
    auto* mock = adapter.get();
    channel::ChannelManager manager{io.get_executor()};
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    auto started = co_await manager.start_all();
    REQUIRE(started.has_value());

    REQUIRE(mock->push_inbound(inbound({core::TextContent{.text = "hello"}})).has_value());
    auto pumped = co_await manager.receive_one("mock-main");
    REQUIRE(pumped.has_value());

    auto failing = channel::ChannelPromptRunner{
        [](channel::ChannelPromptRunRequest) -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
          co_return std::unexpected(core::Error::upstream("agent path failed"));
        }};
    auto receipt = co_await channel::dispatch_one(manager, failing);

    REQUIRE_FALSE(receipt.has_value());
    REQUIRE(receipt.error().kind() == core::ErrorKind::upstream);
    REQUIRE(mock->sent_messages().empty());
  });
}

TEST_CASE("dispatch_one rejects a null runner without consuming messages", "[unit][channel][dispatch][async]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto adapter = std::make_unique<channel::MockChannel>(io.get_executor());
    auto* mock = adapter.get();
    channel::ChannelManager manager{io.get_executor()};
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    auto started = co_await manager.start_all();
    REQUIRE(started.has_value());

    REQUIRE(mock->push_inbound(inbound({core::TextContent{.text = "hello"}})).has_value());
    auto pumped = co_await manager.receive_one("mock-main");
    REQUIRE(pumped.has_value());

    auto receipt = co_await channel::dispatch_one(manager, channel::ChannelPromptRunner{});

    REQUIRE_FALSE(receipt.has_value());
    REQUIRE(receipt.error().kind() == core::ErrorKind::invalid_argument);
    REQUIRE(manager.inbound().size() == 1);
  });
}

TEST_CASE("dispatch_one reports send failures from the owning adapter", "[unit][channel][dispatch][async]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto adapter = std::make_unique<channel::MockChannel>(io.get_executor());
    auto* mock = adapter.get();
    channel::ChannelManager manager{io.get_executor()};
    REQUIRE(manager.register_adapter(std::move(adapter)).has_value());
    auto started = co_await manager.start_all();
    REQUIRE(started.has_value());

    REQUIRE(mock->push_inbound(inbound({core::TextContent{.text = "hello"}})).has_value());
    auto pumped = co_await manager.receive_one("mock-main");
    REQUIRE(pumped.has_value());

    auto stopped = co_await mock->stop();
    REQUIRE(stopped.has_value());

    std::vector<channel::ChannelPromptRunRequest> seen;
    auto receipt = co_await channel::dispatch_one(manager, echo_runner(seen));

    REQUIRE_FALSE(receipt.has_value());
    REQUIRE(receipt.error().kind() == core::ErrorKind::conflict);
    REQUIRE(seen.size() == 1);
  });
}

TEST_CASE("dispatch_one is cancel-aware while awaiting an empty queue", "[unit][channel][dispatch][async]") {
  asio::io_context io;
  channel::ChannelManager manager{io.get_executor()};

  std::vector<channel::ChannelPromptRunRequest> seen;
  asio::cancellation_signal signal;
  std::optional<core::Result<channel::DeliveryReceipt>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<channel::DeliveryReceipt>> {
        co_return co_await channel::dispatch_one(manager, echo_runner(seen));
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<channel::DeliveryReceipt> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  REQUIRE(failure == nullptr);
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(seen.empty());
}

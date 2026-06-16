// tests/desktop/test_chat_bridge.cpp — oran-desktop bridge / view-model coverage.
//
// Slice C of the desktop chat-tracer plan
// (docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md). These cases
// pin the always-built, Slint-free bridge pieces — the `ChatViewModel` folding,
// the `DesktopEventSink` translation, and the bounded `ChatBridge` queues +
// cancellation — against a fake provider. Slint rendering is owned by the shell
// (Slice D) and is not exercised here.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/desktop/chat_bridge.hpp>
#include <oran/provider/fake.hpp>
#include <oran/provider/types.hpp>

#include "../test-helpers/run_async.hpp"

namespace orangutan::desktop {

namespace {

UiUpdate text(std::string_view delta) {
  return UiUpdate{.kind = UiUpdateKind::text_delta, .text = std::string{delta}};
}

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace prov = orangutan::provider;
namespace test = orangutan::tests;

prov::Route fake_route() {
  return prov::Route{
      .primary = prov::ModelTarget{.profile = "fake",
                                   .model = "fake-1",
                                   .protocol = prov::ProtocolKind::anthropic_messages,
                                   .thinking_budget = std::nullopt,
                                   .cache = std::nullopt},
      .fallbacks = {},
  };
}

}  // namespace

TEST_CASE("ChatViewModel opens a streaming assistant turn on submit", "[desktop][view-model]") {
  ChatViewModel vm;
  REQUIRE(vm.status() == TurnStatus::idle);

  vm.submit_user("hello");

  REQUIRE(vm.status() == TurnStatus::streaming);
  REQUIRE(vm.lines().size() == 2);
  REQUIRE(vm.lines()[0].role == ChatLine::Role::user);
  REQUIRE(vm.lines()[0].text == "hello");
  REQUIRE(vm.lines()[1].role == ChatLine::Role::assistant);
  REQUIRE(vm.lines()[1].text.empty());
  REQUIRE(vm.streaming_text().empty());
}

TEST_CASE("ChatViewModel appends answer deltas to the assistant line", "[desktop][view-model]") {
  ChatViewModel vm;
  vm.submit_user("hi");

  vm.apply(text("Hel"));
  vm.apply(text("lo"));

  REQUIRE(vm.lines().size() == 2);
  REQUIRE(vm.lines()[1].text == "Hello");
  REQUIRE(vm.streaming_text() == "Hello");
  REQUIRE(vm.status() == TurnStatus::streaming);
}

TEST_CASE("ChatViewModel keeps thinking deltas out of the answer", "[desktop][view-model]") {
  ChatViewModel vm;
  vm.submit_user("hi");

  vm.apply(UiUpdate{.kind = UiUpdateKind::thinking_delta, .text = "weighing"});
  vm.apply(text("answer"));

  REQUIRE(vm.thinking_text() == "weighing");
  REQUIRE(vm.lines()[1].text == "answer");
}

TEST_CASE("ChatViewModel records tool starts during a turn", "[desktop][view-model]") {
  ChatViewModel vm;
  vm.submit_user("hi");

  vm.apply(UiUpdate{.kind = UiUpdateKind::tool_start, .text = "FileRead", .tool_id = "tu_1"});

  REQUIRE(vm.tool_calls().size() == 1);
  REQUIRE(vm.tool_calls().front() == "FileRead");
  REQUIRE(vm.status() == TurnStatus::streaming);
}

TEST_CASE("ChatViewModel finalizes the turn on done", "[desktop][view-model]") {
  ChatViewModel vm;
  vm.submit_user("hi");
  vm.apply(text("done text"));

  vm.apply(UiUpdate{.kind = UiUpdateKind::done, .stop_reason = core::StopReason::end_turn});

  REQUIRE(vm.status() == TurnStatus::done);
  REQUIRE(vm.lines()[1].text == "done text");
}

TEST_CASE("ChatViewModel surfaces an error update", "[desktop][view-model]") {
  ChatViewModel vm;
  vm.submit_user("hi");

  vm.apply(UiUpdate{.kind = UiUpdateKind::error, .text = "provider exploded"});

  REQUIRE(vm.status() == TurnStatus::error);
  REQUIRE(vm.error_message() == "provider exploded");
}

TEST_CASE("ChatViewModel reopens a streaming turn on the next submit", "[desktop][view-model]") {
  ChatViewModel vm;
  vm.submit_user("first");
  vm.apply(text("one"));
  vm.apply(UiUpdate{.kind = UiUpdateKind::done, .stop_reason = core::StopReason::end_turn});

  vm.submit_user("second");

  REQUIRE(vm.status() == TurnStatus::streaming);
  REQUIRE(vm.lines().size() == 4);
  REQUIRE(vm.lines()[2].text == "second");
  REQUIRE(vm.lines()[3].text.empty());
  REQUIRE(vm.streaming_text().empty());
  REQUIRE(vm.thinking_text().empty());
  REQUIRE(vm.tool_calls().empty());
}

TEST_CASE("DesktopEventSink translates streamed callbacks into updates", "[desktop][sink]") {
  std::vector<UiUpdate> captured;
  DesktopEventSink sink{[&captured](UiUpdate update) { captured.push_back(std::move(update)); }};

  sink.on_text_delta("Hel");
  sink.on_text_delta("lo");
  sink.on_thinking_delta("weighing");
  sink.on_tool_start("tu_1", "FileRead");
  sink.on_done(core::StopReason::end_turn);

  REQUIRE(captured.size() == 5);
  REQUIRE(captured[0] == UiUpdate{.kind = UiUpdateKind::text_delta, .text = "Hel"});
  REQUIRE(captured[1] == UiUpdate{.kind = UiUpdateKind::text_delta, .text = "lo"});
  REQUIRE(captured[2] == UiUpdate{.kind = UiUpdateKind::thinking_delta, .text = "weighing"});
  REQUIRE(captured[3] == UiUpdate{.kind = UiUpdateKind::tool_start, .text = "FileRead", .tool_id = "tu_1"});
  REQUIRE(captured[4] == UiUpdate{.kind = UiUpdateKind::done, .stop_reason = core::StopReason::end_turn});
  REQUIRE(sink.updates_delivered() == 5);
}

TEST_CASE("DesktopEventSink drives a ChatViewModel through its delivery hook", "[desktop][sink]") {
  ChatViewModel vm;
  DesktopEventSink sink{[&vm](const UiUpdate& update) { vm.apply(update); }};

  vm.submit_user("hi");
  sink.on_text_delta("Hello");
  sink.on_done(core::StopReason::end_turn);

  REQUIRE(vm.lines().size() == 2);
  REQUIRE(vm.lines()[1].text == "Hello");
  REQUIRE(vm.status() == TurnStatus::done);
}

TEST_CASE("ChatBridge carries a submitted prompt to the runtime side", "[desktop][bridge]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    ChatBridge bridge{ChatBridgeOptions{.executor = io.get_executor()}};

    REQUIRE(bridge.submit("draft a haiku").has_value());

    auto prompt = co_await bridge.next_prompt();
    REQUIRE(prompt.has_value());
    REQUIRE(*prompt == "draft a haiku");
  });
}

TEST_CASE("ChatBridge marshals provider deltas through its sink into the view-model", "[desktop][bridge]") {
  asio::io_context io;
  ChatBridge bridge{ChatBridgeOptions{.executor = io.get_executor()}};
  ChatViewModel vm;
  vm.submit_user("hi");

  // The provider drives the bridge's sink on the runtime side; the bounded
  // runtime->UI queue buffers updates until the UI thread drains them.
  auto* sink = bridge.event_sink();
  sink->on_text_delta("Hel");
  sink->on_text_delta("lo");
  sink->on_done(core::StopReason::end_turn);

  const auto drained = bridge.drain(vm);

  REQUIRE(drained == 3);
  REQUIRE(vm.lines()[1].text == "Hello");
  REQUIRE(vm.status() == TurnStatus::done);
  REQUIRE(bridge.updates_dropped() == 0);
}

TEST_CASE("ChatBridge counts updates dropped when the runtime->UI queue overflows", "[desktop][bridge]") {
  asio::io_context io;
  ChatBridge bridge{ChatBridgeOptions{.executor = io.get_executor(), .prompt_capacity = 4, .update_capacity = 2}};

  auto* sink = bridge.event_sink();
  sink->on_text_delta("a");
  sink->on_text_delta("b");
  sink->on_text_delta("c");  // overflows the capacity-2 queue
  sink->on_text_delta("d");

  REQUIRE(bridge.updates_dropped() == 2);

  ChatViewModel vm;
  vm.submit_user("hi");
  REQUIRE(bridge.drain(vm) == 2);
}

TEST_CASE("ChatBridge stop request cancels a pending prompt wait", "[desktop][bridge]") {
  asio::io_context io;
  ChatBridge bridge{ChatBridgeOptions{.executor = io.get_executor()}};

  std::optional<core::Result<std::string>> result;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<std::string>> { co_return co_await bridge.next_prompt(); },
      asio::bind_cancellation_slot(bridge.cancellation_slot(),
                                   [&](std::exception_ptr ep, core::Result<std::string> value) {
                                     if (ep) {
                                       std::rethrow_exception(ep);
                                     }
                                     result = std::move(value);
                                     io.stop();
                                   }));

  asio::post(io, [&] { bridge.request_stop(); });
  io.run();

  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
}

TEST_CASE("ChatBridge streams a fake provider turn end-to-end into the view-model", "[desktop][bridge]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    std::vector<prov::ScriptedTurn> plan;
    plan.push_back(prov::ScriptedTurn{
        .response = std::nullopt,
        .deltas = {prov::TextDelta{.text = "Hel"},
                   prov::TextDelta{.text = "lo"},
                   prov::ToolStart{.id = "tu_1", .name = "FileRead"},
                   prov::StreamEnd{.stop_reason = core::StopReason::end_turn,
                                   .usage = std::nullopt,
                                   .model_used = std::nullopt}},
        .error = std::nullopt,
        .latency = {},
    });
    prov::FakeProvider fake{std::move(plan)};

    ChatBridge bridge{ChatBridgeOptions{.executor = io.get_executor()}};
    ChatViewModel vm;
    vm.submit_user("greet me");

    auto response = co_await fake.send(prov::Request{}, fake_route(), bridge.event_sink());
    REQUIRE(response.has_value());

    const auto drained = bridge.drain(vm);
    REQUIRE(drained == 4);
    REQUIRE(vm.lines()[1].text == "Hello");
    REQUIRE(vm.tool_calls().size() == 1);
    REQUIRE(vm.tool_calls().front() == "FileRead");
    REQUIRE(vm.status() == TurnStatus::done);
  });
}

}  // namespace orangutan::desktop

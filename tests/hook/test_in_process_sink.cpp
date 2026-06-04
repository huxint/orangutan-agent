// tests/hook/test_in_process_sink.cpp — `hook::InProcessSink` behavior.

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/hook.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace test = orangutan::tests;

namespace {

hook::ToolBeforePayload sample_before() {
  return hook::ToolBeforePayload{
      .tool_name = "noop",
      .input_json = "{}",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .started_at = core::Time::epoch(),
  };
}

}  // namespace

TEST_CASE("InProcessSink reports its construction id", "[hook][in-process-sink]") {
  hook::InProcessSink sink{"recorder", [](hook::Event, hook::PayloadPtr) -> async::Awaitable<core::Result<void>> {
                             co_return core::Result<void>{};
                           }};
  REQUIRE(sink.id() == "recorder");
  REQUIRE(sink.kind() == hook::SinkKind::default_);
}

TEST_CASE("InProcessSink may opt into trusted-local delivery", "[hook][in-process-sink]") {
  hook::InProcessSink sink{
      "trusted",
      [](hook::Event, hook::PayloadPtr) -> async::Awaitable<core::Result<void>> { co_return core::Result<void>{}; },
      hook::SinkKind::trusted_local};
  REQUIRE(sink.id() == "trusted");
  REQUIRE(sink.kind() == hook::SinkKind::trusted_local);
}

TEST_CASE("InProcessSink forwards event + payload to the callback", "[hook][in-process-sink]") {
  hook::Event captured_event{};
  std::string captured_tool;
  hook::InProcessSink sink{"recorder",
                           [&](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
                             captured_event = event;
                             if (auto* before = std::get_if<hook::ToolBeforePayload>(payload.get());
                                 before != nullptr) {
                               captured_tool = before->tool_name;
                             }
                             co_return core::Result<void>{};
                           }};

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await sink.receive(hook::Event::tool_before, std::make_shared<hook::Payload>(sample_before()));
    REQUIRE(result.has_value());
    co_return;
  });

  REQUIRE(captured_event == hook::Event::tool_before);
  REQUIRE(captured_tool == "noop");
}

TEST_CASE("InProcessSink propagates a callback error verbatim", "[hook][in-process-sink]") {
  hook::InProcessSink sink{"failing", [](hook::Event, hook::PayloadPtr) -> async::Awaitable<core::Result<void>> {
                             co_return std::unexpected(
                                 core::Error::internal("callback rejected").with("sink", "failing"));
                           }};

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await sink.receive(hook::Event::tool_before, std::make_shared<hook::Payload>(sample_before()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message() == "callback rejected");
    co_return;
  });
}

TEST_CASE("InProcessSink with an empty callback returns invalid_argument", "[hook][in-process-sink]") {
  hook::InProcessSink sink{"empty", hook::InProcessSink::Callback{}};

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await sink.receive(hook::Event::tool_before, std::make_shared<hook::Payload>(sample_before()));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
    co_return;
  });
}

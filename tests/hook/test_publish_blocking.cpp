// tests/hook/test_publish_blocking.cpp — `hook::Bus::publish_blocking<E>`
// coverage for spec 0015 v1.
//
// The test matrix follows the v1 acceptance criteria sequence: the API
// surface (no-sink default proceed, single-sink each decision kind,
// multi-sink resolution order) plus the failure classification (sink
// returns error -> veto, sink throws -> veto). The dispatch-pipeline
// consumer (`Registry::dispatch` consumption) lands in a follow-up
// slice, so this bucket only exercises the bus-side contract.

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>

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

using namespace std::chrono_literals;

[[nodiscard]] hook::ToolBeforePayload sample_before() {
  return hook::ToolBeforePayload{
      .tool_name = "noop",
      .input_json = "{}",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .started_at = core::Time::epoch(),
  };
}

[[nodiscard]] hook::ToolBeforePayload sample_before_with_redacted_input() {
  return hook::ToolBeforePayload{
      .tool_name = "file.edit",
      .input_json = R"({"path":"notes.md","old_string":"secret","new_string":"public"})",
      .redacted_input_json =
          R"({"kind":"redacted_tool_input","input_hash":"abc","old_string_bytes":6,"new_string_bytes":6})",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .started_at = core::Time::epoch(),
  };
}

/// `Sink` whose `handle_blocking` records every call and yields a
/// caller-supplied decision. The advisory `receive` path is unused here
/// but must remain implementable since `Sink::receive` is pure virtual.
class BlockingSink final : public hook::Sink {
public:
  BlockingSink(std::string id, hook::HookDecision decision) : id_(std::move(id)), decision_(std::move(decision)) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event /*event*/,
                                                             hook::Payload /*payload*/) override {
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<hook::HookDecision>> handle_blocking(hook::Event /*event*/,
                                                                                   hook::Payload /*payload*/) override {
    ++calls_;
    co_return decision_;
  }

  [[nodiscard]] std::size_t calls() const noexcept {
    return calls_;
  }

private:
  std::string id_;
  hook::HookDecision decision_;
  std::size_t calls_{0};
};

/// `Sink` whose `handle_blocking` returns a `core::Result` error.
class FailingBlockingSink final : public hook::Sink {
public:
  FailingBlockingSink(std::string id, std::string reason) : id_(std::move(id)), reason_(std::move(reason)) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event /*event*/,
                                                             hook::Payload /*payload*/) override {
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<hook::HookDecision>> handle_blocking(hook::Event /*event*/,
                                                                                   hook::Payload /*payload*/) override {
    co_return std::unexpected(core::Error::internal(reason_));
  }

private:
  std::string id_;
  std::string reason_;
};

/// Cancellation-aware sink that stays inside `handle_blocking` longer
/// than the bus timeout. The bus should synthesize `hook_timeout` and
/// cancel this awaitable before the test helper's hard timeout fires.
class SlowBlockingSink final : public hook::Sink {
public:
  SlowBlockingSink(std::string id, std::chrono::milliseconds delay) : id_(std::move(id)), delay_(delay) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event /*event*/,
                                                             hook::Payload /*payload*/) override {
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<hook::HookDecision>> handle_blocking(hook::Event /*event*/,
                                                                                   hook::Payload /*payload*/) override {
    ++calls_;
    const auto executor = co_await asio::this_coro::executor;
    auto slept = co_await async::sleep_for(executor, delay_);
    if (!slept) {
      co_return std::unexpected(std::move(slept).error());
    }
    co_return hook::HookDecision{};
  }

  [[nodiscard]] std::size_t calls() const noexcept {
    return calls_;
  }

private:
  std::string id_;
  std::chrono::milliseconds delay_;
  std::size_t calls_{0};
};

/// `Sink` whose `handle_blocking` throws an exception from its
/// awaitable. Mirrors the existing throwing-sink coverage in
/// `test_bus.cpp` for advisory publishes.
class ThrowingBlockingSink final : public hook::Sink {
public:
  ThrowingBlockingSink(std::string id, std::string reason) : id_(std::move(id)), reason_(std::move(reason)) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event /*event*/,
                                                             hook::Payload /*payload*/) override {
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<hook::HookDecision>> handle_blocking(hook::Event /*event*/,
                                                                                   hook::Payload /*payload*/) override {
    throw std::runtime_error{reason_};
    co_return hook::HookDecision{};
  }

private:
  std::string id_;
  std::string reason_;
};

}  // namespace

TEST_CASE("publish_blocking on empty bus returns proceed", "[hook][bus][blocking]") {
  hook::Bus bus;
  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::proceed);
    REQUIRE(result->reason.empty());
    REQUIRE(result->trace.empty());
    co_return;
  });
}

TEST_CASE("Sink default handle_blocking returns proceed", "[hook][bus][blocking]") {
  hook::Bus bus;
  // An InProcessSink without a blocking handler installed falls back to
  // Sink::handle_blocking, which yields HookDecision{} (proceed).
  hook::InProcessSink sink{
      "default-sink",
      [](hook::Event /*event*/, hook::Payload /*payload*/) -> async::Awaitable<core::Result<void>> {
        co_return core::Result<void>{};
      }};
  bus.bind(sink, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::proceed);
    co_return;
  });
}

TEST_CASE("publish_blocking returns single sink's veto decision", "[hook][bus][blocking]") {
  hook::Bus bus;
  hook::HookDecision veto{};
  veto.kind = hook::HookDecisionKind::veto;
  veto.reason = "policy";
  BlockingSink sink{"policy-sink", veto};
  bus.bind(sink, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::veto);
    REQUIRE(result->reason == "policy");
    REQUIRE(result->trace.size() == 1);
    REQUIRE(result->trace[0].sink_id == "policy-sink");
    REQUIRE(result->trace[0].kind == hook::HookDecisionKind::veto);
    REQUIRE(result->trace[0].reason == "policy");
    co_return;
  });

  REQUIRE(sink.calls() == 1);
}

TEST_CASE("publish_blocking returns single sink's rewrite decision", "[hook][bus][blocking]") {
  hook::Bus bus;
  hook::HookDecision rewrite{};
  rewrite.kind = hook::HookDecisionKind::rewrite;
  rewrite.reason = "narrow_path";
  rewrite.rewritten_input_json = std::string{R"({"path":"src/main.cpp"})"};
  BlockingSink sink{"rewriter", rewrite};
  bus.bind(sink, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::rewrite);
    REQUIRE(result->reason == "narrow_path");
    REQUIRE(result->rewritten_input_json == R"({"path":"src/main.cpp"})");
    co_return;
  });
}

TEST_CASE("publish_blocking returns single sink's require_approval decision", "[hook][bus][blocking]") {
  hook::Bus bus;
  hook::HookDecision require{};
  require.kind = hook::HookDecisionKind::require_approval;
  require.reason = "operator_review";
  BlockingSink sink{"approval-router", require};
  bus.bind(sink, {hook::Event::permission_ask_rendered});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::permission_ask_rendered>(std::monostate{});
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::require_approval);
    REQUIRE(result->reason == "operator_review");
    co_return;
  });
}

TEST_CASE("publish_blocking short-circuits at first non-proceed sink", "[hook][bus][blocking]") {
  hook::Bus bus;
  hook::HookDecision proceed{};
  hook::HookDecision veto{};
  veto.kind = hook::HookDecisionKind::veto;
  veto.reason = "first-veto";
  BlockingSink first{"first", veto};
  BlockingSink second{"second", proceed};
  BlockingSink third{"third", proceed};
  bus.bind(first, {hook::Event::tool_before});
  bus.bind(second, {hook::Event::tool_before});
  bus.bind(third, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::veto);
    REQUIRE(result->reason == "first-veto");
    REQUIRE(result->trace.size() == 1);
    REQUIRE(result->trace[0].sink_id == "first");
    REQUIRE(result->trace[0].kind == hook::HookDecisionKind::veto);
    co_return;
  });

  REQUIRE(first.calls() == 1);
  REQUIRE(second.calls() == 0);
  REQUIRE(third.calls() == 0);
}

TEST_CASE("publish_blocking consults later sink when earlier returns proceed", "[hook][bus][blocking]") {
  hook::Bus bus;
  hook::HookDecision proceed{};
  hook::HookDecision require{};
  require.kind = hook::HookDecisionKind::require_approval;
  require.reason = "second-required";
  BlockingSink first{"first", proceed};
  BlockingSink second{"second", require};
  bus.bind(first, {hook::Event::tool_before});
  bus.bind(second, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::require_approval);
    REQUIRE(result->reason == "second-required");
    REQUIRE(result->trace.size() == 2);
    REQUIRE(result->trace[0].sink_id == "first");
    REQUIRE(result->trace[0].kind == hook::HookDecisionKind::proceed);
    REQUIRE(result->trace[1].sink_id == "second");
    REQUIRE(result->trace[1].kind == hook::HookDecisionKind::require_approval);
    co_return;
  });

  REQUIRE(first.calls() == 1);
  REQUIRE(second.calls() == 1);
}

TEST_CASE("publish_blocking returns proceed when all sinks proceed", "[hook][bus][blocking]") {
  hook::Bus bus;
  hook::HookDecision proceed{};
  BlockingSink first{"first", proceed};
  BlockingSink second{"second", proceed};
  bus.bind(first, {hook::Event::tool_before});
  bus.bind(second, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::proceed);
    REQUIRE(result->trace.size() == 2);
    REQUIRE(result->trace[0].sink_id == "first");
    REQUIRE(result->trace[0].kind == hook::HookDecisionKind::proceed);
    REQUIRE(result->trace[1].sink_id == "second");
    REQUIRE(result->trace[1].kind == hook::HookDecisionKind::proceed);
    co_return;
  });

  REQUIRE(first.calls() == 1);
  REQUIRE(second.calls() == 1);
}

TEST_CASE("sink error becomes veto with reason=hook_error", "[hook][bus][blocking]") {
  hook::Bus bus;
  FailingBlockingSink first{"failing", "boom"};
  hook::HookDecision proceed{};
  BlockingSink second{"second", proceed};
  bus.bind(first, {hook::Event::tool_before});
  bus.bind(second, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::veto);
    REQUIRE(result->reason.starts_with("hook_error"));
    REQUIRE(result->reason.contains("boom"));
    REQUIRE(result->reason.contains("[sink=failing]"));
    REQUIRE(result->trace.size() == 1);
    REQUIRE(result->trace[0].sink_id == "failing");
    REQUIRE(result->trace[0].kind == hook::HookDecisionKind::veto);
    co_return;
  });

  // Second sink not called: the failing sink short-circuited the walk.
  // (BlockingSink::calls is the call counter; second has none.)
}

TEST_CASE("throwing sink becomes veto with reason=hook_error", "[hook][bus][blocking]") {
  hook::Bus bus;
  ThrowingBlockingSink first{"thrower", "kaboom"};
  bus.bind(first, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::veto);
    REQUIRE(result->reason.starts_with("hook_error"));
    REQUIRE(result->reason.contains("kaboom"));
    REQUIRE(result->reason.contains("[sink=thrower]"));
    REQUIRE(result->trace.size() == 1);
    REQUIRE(result->trace[0].sink_id == "thrower");
    REQUIRE(result->trace[0].kind == hook::HookDecisionKind::veto);
    co_return;
  });
}

TEST_CASE("blocking sink timeout becomes veto with elapsed trace", "[hook][bus][blocking]") {
  hook::Bus bus{hook::BusOptions{.blocking_timeout = 5ms}};
  SlowBlockingSink first{"slow", 1s};
  hook::HookDecision proceed{};
  BlockingSink second{"second", proceed};
  bus.bind(first, {hook::Event::tool_before});
  bus.bind(second, {hook::Event::tool_before});

  test::run_async(
      [&](asio::io_context& /*io*/) -> async::Awaitable<void> {
        auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
        REQUIRE(result.has_value());
        REQUIRE(result->kind == hook::HookDecisionKind::veto);
        REQUIRE(result->reason == "hook_timeout");
        REQUIRE(result->trace.size() == 1);
        REQUIRE(result->trace[0].sink_id == "slow");
        REQUIRE(result->trace[0].kind == hook::HookDecisionKind::veto);
        REQUIRE(result->trace[0].reason == "hook_timeout");
        REQUIRE(result->trace[0].elapsed == 5ms);
        co_return;
      },
      250ms);

  REQUIRE(first.calls() == 1);
  REQUIRE(second.calls() == 0);
}

TEST_CASE("InProcessSink blocking handler drives the decision", "[hook][bus][blocking]") {
  hook::Bus bus;
  hook::InProcessSink sink{
      "in-process",
      [](hook::Event /*event*/, hook::Payload /*payload*/) -> async::Awaitable<core::Result<void>> {
        co_return core::Result<void>{};
      }};
  sink.set_blocking_handler(
      [](hook::Event /*event*/, hook::Payload /*payload*/) -> async::Awaitable<core::Result<hook::HookDecision>> {
        hook::HookDecision decision{};
        decision.kind = hook::HookDecisionKind::rewrite;
        decision.reason = "narrowed";
        decision.rewritten_input_json = std::string{R"({"path":"src/x"})"};
        co_return decision;
      });
  bus.bind(sink, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::rewrite);
    REQUIRE(result->reason == "narrowed");
    REQUIRE(result->rewritten_input_json == R"({"path":"src/x"})");
    co_return;
  });
}

TEST_CASE("publish_blocking redacts input_json when a sanitized view is present", "[hook][bus][blocking][redaction]") {
  hook::Bus bus;
  std::string default_input;
  std::string trusted_input;
  hook::InProcessSink default_sink{
      "default",
      [](hook::Event /*event*/, hook::Payload /*payload*/) -> async::Awaitable<core::Result<void>> {
        co_return core::Result<void>{};
      }};
  default_sink.set_blocking_handler(
      [&](hook::Event /*event*/, hook::Payload payload) -> async::Awaitable<core::Result<hook::HookDecision>> {
        const auto* before = std::get_if<hook::ToolBeforePayload>(&payload);
        REQUIRE(before != nullptr);
        default_input = before->input_json;
        co_return hook::HookDecision{};
      });
  hook::InProcessSink trusted_sink{
      "trusted",
      [](hook::Event /*event*/, hook::Payload /*payload*/) -> async::Awaitable<core::Result<void>> {
        co_return core::Result<void>{};
      },
      hook::SinkKind::trusted_local};
  trusted_sink.set_blocking_handler(
      [&](hook::Event /*event*/, hook::Payload payload) -> async::Awaitable<core::Result<hook::HookDecision>> {
        const auto* before = std::get_if<hook::ToolBeforePayload>(&payload);
        REQUIRE(before != nullptr);
        trusted_input = before->input_json;
        co_return hook::HookDecision{};
      });
  bus.bind(default_sink, {hook::Event::tool_before});
  bus.bind(trusted_sink, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto result = co_await bus.publish_blocking<hook::Event::tool_before>(sample_before_with_redacted_input());
    REQUIRE(result.has_value());
    REQUIRE(result->kind == hook::HookDecisionKind::proceed);
    REQUIRE(result->trace.size() == 2);
    co_return;
  });

  REQUIRE(default_input ==
          R"({"kind":"redacted_tool_input","input_hash":"abc","old_string_bytes":6,"new_string_bytes":6})");
  REQUIRE(trusted_input == R"({"path":"notes.md","old_string":"secret","new_string":"public"})");
}

TEST_CASE("EventTraits encodes the v1 blocking whitelist", "[hook][event][blocking]") {
  STATIC_REQUIRE(hook::HasBlockingDecision<hook::Event::tool_before>);
  STATIC_REQUIRE(hook::HasBlockingDecision<hook::Event::permission_ask_rendered>);
  STATIC_REQUIRE(hook::HasBlockingDecision<hook::Event::memory_write_before>);
  STATIC_REQUIRE_FALSE(hook::HasBlockingDecision<hook::Event::tool_after>);
  STATIC_REQUIRE_FALSE(hook::HasBlockingDecision<hook::Event::iteration_start>);
  STATIC_REQUIRE_FALSE(hook::HasBlockingDecision<hook::Event::memory_read_before>);
  STATIC_REQUIRE_FALSE(hook::HasBlockingDecision<hook::Event::permission_denied>);
}

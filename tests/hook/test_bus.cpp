// tests/hook/test_bus.cpp — `hook::Bus` subscribe/publish coverage.

#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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

/// Recording sink — captures every (event, payload, payload_kind) tuple. The
/// `payload_kind` is a stable string the test can match against.
struct Capture {
  hook::Event event;
  std::string payload_kind;  // "before", "dispatched", "after", "error", "ask", "monostate"
  std::optional<std::string> data_json;
};

[[nodiscard]] std::string payload_kind(const hook::Payload& payload) {
  return std::visit(
      [](const auto& alt) -> std::string {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "monostate";
        } else if constexpr (std::is_same_v<T, hook::ToolBeforePayload>) {
          return "before";
        } else if constexpr (std::is_same_v<T, hook::ToolDispatchedPayload>) {
          return "dispatched";
        } else if constexpr (std::is_same_v<T, hook::ToolAfterPayload>) {
          return "after";
        } else if constexpr (std::is_same_v<T, hook::ToolErrorPayload>) {
          return "error";
        } else if constexpr (std::is_same_v<T, hook::PermissionAskRenderedPayload>) {
          return "ask";
        }
      },
      payload);
}

class RecordingSink final : public hook::Sink {
public:
  explicit RecordingSink(std::string id, hook::SinkKind kind = hook::SinkKind::default_)
      : id_(std::move(id)), kind_(kind) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] hook::SinkKind kind() const noexcept override {
    return kind_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event event, hook::Payload payload) override {
    std::optional<std::string> data_json;
    if (const auto* after = std::get_if<hook::ToolAfterPayload>(&payload); after != nullptr) {
      data_json = after->data_json;
    }
    captures_.push_back({.event = event, .payload_kind = payload_kind(payload), .data_json = std::move(data_json)});
    co_return core::Result<void>{};
  }

  [[nodiscard]] std::span<const Capture> captures() const noexcept {
    return captures_;
  }

private:
  std::string id_;
  hook::SinkKind kind_{hook::SinkKind::default_};
  std::vector<Capture> captures_;
};

class FailingSink final : public hook::Sink {
public:
  explicit FailingSink(std::string id, std::string reason) : id_(std::move(id)), reason_(std::move(reason)) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event /*event*/,
                                                             hook::Payload /*payload*/) override {
    co_return std::unexpected(core::Error::internal(reason_).with("sink", id_));
  }

private:
  std::string id_;
  std::string reason_;
};

/// Sink that throws from its awaitable. Required to verify the advisory
/// contract that one badly-behaved sink (one that propagates an exception
/// rather than returning std::unexpected) does not abort the publish for
/// later sinks and does not propagate the exception out of publish_advisory.
class ThrowingSink final : public hook::Sink {
public:
  explicit ThrowingSink(std::string id, std::string reason) : id_(std::move(id)), reason_(std::move(reason)) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event /*event*/,
                                                             hook::Payload /*payload*/) override {
    throw std::runtime_error{reason_};
    co_return core::Result<void>{};
  }

private:
  std::string id_;
  std::string reason_;
};

hook::ToolBeforePayload sample_before() {
  return hook::ToolBeforePayload{
      .tool_name = "noop",
      .input_json = "{}",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .started_at = core::Time::epoch(),
  };
}

hook::ToolAfterPayload sample_after_with_data() {
  return hook::ToolAfterPayload{
      .tool_name = "noop",
      .input_json = "{}",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .succeeded = true,
      .output_text = "ok",
      .data_json = std::string{R"({"kind":"sample","raw":true})"},
      .error_kind = "",
      .error_message = "",
      .started_at = core::Time::epoch(),
      .finished_at = core::Time::epoch(),
  };
}

}  // namespace

TEST_CASE("Bus is empty by default", "[hook][bus]") {
  hook::Bus bus;
  REQUIRE(bus.binding_count() == 0);
  REQUIRE(bus.sink_count(hook::Event::tool_before) == 0);
  REQUIRE(bus.sink_count(hook::Event::tool_after) == 0);
}

TEST_CASE("publish_advisory on empty bus succeeds with empty outcome", "[hook][bus]") {
  hook::Bus bus;
  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto outcome = co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
    REQUIRE(outcome.sinks.empty());
    REQUIRE(outcome.all_succeeded());
    REQUIRE(outcome.failure_count() == 0);
    co_return;
  });
}

TEST_CASE("bind connects sink to event, publish_advisory drives it", "[hook][bus]") {
  hook::Bus bus;
  RecordingSink sink{"recorder-1"};
  bus.bind(sink, {hook::Event::tool_before});

  REQUIRE(bus.binding_count() == 1);
  REQUIRE(bus.sink_count(hook::Event::tool_before) == 1);
  REQUIRE(bus.sink_count(hook::Event::tool_after) == 0);

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto outcome = co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
    REQUIRE(outcome.sinks.size() == 1);
    REQUIRE(outcome.sinks[0].sink_id == "recorder-1");
    REQUIRE_FALSE(outcome.sinks[0].error.has_value());
    REQUIRE(outcome.all_succeeded());
    co_return;
  });

  REQUIRE(sink.captures().size() == 1);
  REQUIRE(sink.captures()[0].event == hook::Event::tool_before);
  REQUIRE(sink.captures()[0].payload_kind == "before");
}

TEST_CASE("bind to multiple events delivers each separately", "[hook][bus]") {
  hook::Bus bus;
  RecordingSink sink{"recorder-2"};
  bus.bind(sink, {hook::Event::tool_before, hook::Event::tool_after});

  REQUIRE(bus.binding_count() == 2);

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    (void)co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
    (void)co_await bus.publish_advisory(hook::Event::tool_after, std::monostate{});
    co_return;
  });

  REQUIRE(sink.captures().size() == 2);
  REQUIRE(sink.captures()[0].event == hook::Event::tool_before);
  REQUIRE(sink.captures()[0].payload_kind == "before");
  REQUIRE(sink.captures()[1].event == hook::Event::tool_after);
  REQUIRE(sink.captures()[1].payload_kind == "monostate");
}

TEST_CASE("multiple sinks subscribed to same event run in subscription order", "[hook][bus]") {
  hook::Bus bus;
  RecordingSink first{"first"};
  RecordingSink second{"second"};
  RecordingSink third{"third"};
  bus.bind(first, {hook::Event::tool_before});
  bus.bind(second, {hook::Event::tool_before});
  bus.bind(third, {hook::Event::tool_before});

  REQUIRE(bus.sink_count(hook::Event::tool_before) == 3);

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto outcome = co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
    REQUIRE(outcome.sinks.size() == 3);
    REQUIRE(outcome.sinks[0].sink_id == "first");
    REQUIRE(outcome.sinks[1].sink_id == "second");
    REQUIRE(outcome.sinks[2].sink_id == "third");
    co_return;
  });

  REQUIRE(first.captures().size() == 1);
  REQUIRE(second.captures().size() == 1);
  REQUIRE(third.captures().size() == 1);
}

TEST_CASE("publish_advisory redacts tool_after data_json unless sink is trusted-local", "[hook][bus][redaction]") {
  hook::Bus bus;
  RecordingSink default_sink{"default"};
  RecordingSink trusted_sink{"trusted", hook::SinkKind::trusted_local};
  bus.bind(default_sink, {hook::Event::tool_after});
  bus.bind(trusted_sink, {hook::Event::tool_after});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto outcome = co_await bus.publish_advisory(hook::Event::tool_after, sample_after_with_data());
    REQUIRE(outcome.sinks.size() == 2);
    REQUIRE(outcome.all_succeeded());
    co_return;
  });

  REQUIRE(default_sink.captures().size() == 1);
  REQUIRE(default_sink.captures()[0].payload_kind == "after");
  REQUIRE_FALSE(default_sink.captures()[0].data_json.has_value());

  REQUIRE(trusted_sink.captures().size() == 1);
  REQUIRE(trusted_sink.captures()[0].payload_kind == "after");
  REQUIRE(trusted_sink.captures()[0].data_json == R"({"kind":"sample","raw":true})");
}

TEST_CASE("bind is idempotent — duplicate pair does not double-fire", "[hook][bus]") {
  hook::Bus bus;
  RecordingSink sink{"once"};
  bus.bind(sink, {hook::Event::tool_before});
  bus.bind(sink, {hook::Event::tool_before});                           // duplicate
  bus.bind(sink, {hook::Event::tool_before, hook::Event::tool_after});  // partial duplicate

  REQUIRE(bus.sink_count(hook::Event::tool_before) == 1);
  REQUIRE(bus.sink_count(hook::Event::tool_after) == 1);
  REQUIRE(bus.binding_count() == 2);
}

TEST_CASE("unbind removes every subscription a sink holds", "[hook][bus]") {
  hook::Bus bus;
  RecordingSink kept{"kept"};
  RecordingSink removed{"removed"};
  bus.bind(kept, {hook::Event::tool_before, hook::Event::tool_after});
  bus.bind(removed, {hook::Event::tool_before, hook::Event::tool_after});

  REQUIRE(bus.binding_count() == 4);
  REQUIRE(bus.unbind(removed) == 2);
  REQUIRE(bus.binding_count() == 2);
  REQUIRE(bus.sink_count(hook::Event::tool_before) == 1);
  REQUIRE(bus.sink_count(hook::Event::tool_after) == 1);

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    (void)co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
    co_return;
  });

  REQUIRE(kept.captures().size() == 1);
  REQUIRE(removed.captures().empty());
}

TEST_CASE("unbind of unsubscribed sink reports zero removed", "[hook][bus]") {
  hook::Bus bus;
  RecordingSink lurker{"lurker"};
  REQUIRE(bus.unbind(lurker) == 0);
}

TEST_CASE("sink errors are captured in outcome but do not abort the publish", "[hook][bus]") {
  hook::Bus bus;
  FailingSink first{"first", "boom"};
  RecordingSink second{"second"};
  FailingSink third{"third", "kaboom"};
  bus.bind(first, {hook::Event::tool_before});
  bus.bind(second, {hook::Event::tool_before});
  bus.bind(third, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto outcome = co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
    REQUIRE(outcome.sinks.size() == 3);
    REQUIRE_FALSE(outcome.all_succeeded());
    REQUIRE(outcome.failure_count() == 2);

    REQUIRE(outcome.sinks[0].sink_id == "first");
    REQUIRE(outcome.sinks[0].error.has_value());
    REQUIRE(outcome.sinks[0].error->message() == "boom");

    REQUIRE(outcome.sinks[1].sink_id == "second");
    REQUIRE_FALSE(outcome.sinks[1].error.has_value());

    REQUIRE(outcome.sinks[2].sink_id == "third");
    REQUIRE(outcome.sinks[2].error.has_value());
    REQUIRE(outcome.sinks[2].error->message() == "kaboom");
    co_return;
  });

  // The middle sink ran despite both neighbours erroring.
  REQUIRE(second.captures().size() == 1);
}

TEST_CASE("throwing sink does not abort publish — exception captured as Error::internal", "[hook][bus]") {
  hook::Bus bus;
  ThrowingSink first{"first", "stdexcept-boom"};
  RecordingSink second{"second"};
  ThrowingSink third{"third", "another-boom"};
  bus.bind(first, {hook::Event::tool_before});
  bus.bind(second, {hook::Event::tool_before});
  bus.bind(third, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto outcome = co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
    REQUIRE(outcome.sinks.size() == 3);
    REQUIRE_FALSE(outcome.all_succeeded());
    REQUIRE(outcome.failure_count() == 2);

    REQUIRE(outcome.sinks[0].sink_id == "first");
    REQUIRE(outcome.sinks[0].error.has_value());
    REQUIRE(outcome.sinks[0].error->kind() == core::ErrorKind::internal);
    REQUIRE(outcome.sinks[0].error->message() == "stdexcept-boom");
    // The catch handler attaches the sink id as structured context so the
    // operator can tell which extension threw without re-parsing message.
    REQUIRE(outcome.sinks[0].error->context().size() == 1);
    REQUIRE(outcome.sinks[0].error->context()[0].first == "sink");
    REQUIRE(outcome.sinks[0].error->context()[0].second == "first");

    REQUIRE(outcome.sinks[1].sink_id == "second");
    REQUIRE_FALSE(outcome.sinks[1].error.has_value());

    REQUIRE(outcome.sinks[2].sink_id == "third");
    REQUIRE(outcome.sinks[2].error.has_value());
    REQUIRE(outcome.sinks[2].error->kind() == core::ErrorKind::internal);
    REQUIRE(outcome.sinks[2].error->message() == "another-boom");
    co_return;
  });

  // The middle sink ran even though both neighbours threw — the advisory
  // contract demands it. Before slice 31, the first throw would have
  // escaped publish_advisory entirely and crashed tool dispatch.
  REQUIRE(second.captures().size() == 1);
}

TEST_CASE("default_mode reports blocking for known pre-action events", "[hook][event]") {
  REQUIRE(hook::default_mode(hook::Event::tool_before) == hook::Mode::blocking);
  REQUIRE(hook::default_mode(hook::Event::memory_write_before) == hook::Mode::blocking);
  REQUIRE(hook::default_mode(hook::Event::memory_read_before) == hook::Mode::blocking);
  REQUIRE(hook::default_mode(hook::Event::permission_ask_rendered) == hook::Mode::blocking);

  // Advisory events:
  REQUIRE(hook::default_mode(hook::Event::tool_after) == hook::Mode::advisory);
  REQUIRE(hook::default_mode(hook::Event::tool_error) == hook::Mode::advisory);
  REQUIRE(hook::default_mode(hook::Event::iteration_start) == hook::Mode::advisory);
  REQUIRE(hook::default_mode(hook::Event::provider_request) == hook::Mode::advisory);
  REQUIRE(hook::default_mode(hook::Event::permission_denied) == hook::Mode::advisory);
}

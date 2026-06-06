// tests/hook/test_bus.cpp — `hook::Bus` subscribe/publish coverage.

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
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

/// Recording sink — captures every (event, payload, payload_kind) tuple. The
/// `payload_kind` is a stable string the test can match against.
struct Capture {
  hook::Event event;
  std::string payload_kind;  // "before", "dispatched", "after", "error", "ask", memory*, provider*, "monostate"
  std::string input_json;
  std::optional<std::string> data_json;
};

[[nodiscard]] std::string payload_kind(const hook::Payload& payload) {
  return std::visit(
      [](const auto& alt) -> std::string {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::same_as<T, std::monostate>) {
          return "monostate";
        } else if constexpr (std::same_as<T, hook::ToolBeforePayload>) {
          return "before";
        } else if constexpr (std::same_as<T, hook::ToolDispatchedPayload>) {
          return "dispatched";
        } else if constexpr (std::same_as<T, hook::ToolAfterPayload>) {
          return "after";
        } else if constexpr (std::same_as<T, hook::ToolErrorPayload>) {
          return "error";
        } else if constexpr (std::same_as<T, hook::PermissionAskRenderedPayload>) {
          return "ask";
        } else if constexpr (std::same_as<T, hook::MemoryReadPayload>) {
          return "memory_read";
        } else if constexpr (std::same_as<T, hook::MemoryWritePayload>) {
          return "memory_write";
        } else if constexpr (std::same_as<T, hook::MemoryForgetPayload>) {
          return "memory_forget";
        } else if constexpr (std::same_as<T, hook::MemoryDecayPayload>) {
          return "memory_decay";
        } else if constexpr (std::same_as<T, hook::ProviderRequestPayload>) {
          return "provider_request";
        } else if constexpr (std::same_as<T, hook::ProviderResponsePayload>) {
          return "provider_response";
        } else if constexpr (std::same_as<T, hook::ProviderErrorPayload>) {
          return "provider_error";
        } else if constexpr (std::same_as<T, hook::ProviderFallbackPayload>) {
          return "provider_fallback";
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

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event event, hook::PayloadPtr payload) override {
    std::string input_json;
    std::optional<std::string> data_json;
    std::visit(
        [&](const auto& alt) {
          if constexpr (requires { alt.input_json; }) {
            input_json = alt.input_json;
          }
        },
        *payload);
    if (const auto* after = std::get_if<hook::ToolAfterPayload>(payload.get()); after != nullptr) {
      data_json = after->data_json;
    }
    captures_.push_back({.event = event,
                         .payload_kind = payload_kind(*payload),
                         .input_json = std::move(input_json),
                         .data_json = std::move(data_json)});
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
                                                             hook::PayloadPtr /*payload*/) override {
    co_return std::unexpected(core::Error::internal(reason_).with("sink", id_));
  }

private:
  std::string id_;
  std::string reason_;
};

class DelayedSink final : public hook::Sink {
public:
  DelayedSink(std::string id,
              std::chrono::milliseconds delay,
              std::size_t& active,
              std::size_t& peak_active,
              std::vector<std::string>& completions)
      : id_(std::move(id)), delay_(delay), active_(&active), peak_active_(&peak_active), completions_(&completions) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event /*event*/,
                                                             hook::PayloadPtr /*payload*/) override {
    ++(*active_);
    *peak_active_ = std::max(*peak_active_, *active_);
    const auto executor = co_await asio::this_coro::executor;
    auto slept = co_await async::sleep_for(executor, delay_);
    --(*active_);
    if (!slept) {
      co_return std::unexpected(std::move(slept).error());
    }
    completions_->push_back(id_);
    co_return core::Result<void>{};
  }

private:
  std::string id_;
  std::chrono::milliseconds delay_;
  std::size_t* active_;
  std::size_t* peak_active_;
  std::vector<std::string>* completions_;
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
                                                             hook::PayloadPtr /*payload*/) override {
    throw std::runtime_error{reason_};
    co_return core::Result<void>{};
  }

private:
  std::string id_;
  std::string reason_;
};

class PayloadPointerSink final : public hook::Sink {
public:
  PayloadPointerSink(std::string id,
                     std::vector<const hook::Payload*>& payloads,
                     std::vector<std::string>& inputs,
                     hook::SinkKind kind = hook::SinkKind::default_)
      : id_(std::move(id)), payloads_(&payloads), inputs_(&inputs), kind_(kind) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] hook::SinkKind kind() const noexcept override {
    return kind_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event /*event*/, hook::PayloadPtr payload) override {
    payloads_->push_back(payload.get());
    std::visit(
        [&](const auto& alt) {
          if constexpr (requires { alt.input_json; }) {
            inputs_->push_back(alt.input_json);
          }
        },
        *payload);
    co_return core::Result<void>{};
  }

private:
  std::string id_;
  std::vector<const hook::Payload*>* payloads_;
  std::vector<std::string>* inputs_;
  hook::SinkKind kind_{hook::SinkKind::default_};
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

hook::ToolAfterPayload sample_after_with_redacted_input() {
  return hook::ToolAfterPayload{
      .tool_name = "file.write",
      .input_json = R"({"path":"notes.md","content":"secret"})",
      .redacted_input_json = R"({"kind":"redacted_tool_input","input_hash":"abc","content_bytes":6})",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .succeeded = true,
      .output_text = "ok",
      .error_kind = "",
      .error_message = "",
      .started_at = core::Time::epoch(),
      .finished_at = core::Time::epoch(),
  };
}

hook::MemoryReadPayload sample_memory_read() {
  return hook::MemoryReadPayload{
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .source = "memory.recall",
      .query = "sensitive query",
      .redacted_query_bytes = std::string_view{"sensitive query"}.size(),
      .limit = 5,
      .kinds = {"project"},
      .match_count = 1,
      .hits =
          {
              hook::MemoryReadHitPayload{
                  .record =
                      hook::MemoryRecordPayload{
                          .id = "memory-1",
                          .scope_key = "scope",
                          .kind = "project",
                          .title = "Sensitive title",
                          .body = "Sensitive body",
                          .created_at = core::Time::epoch(),
                          .updated_at = core::Time::epoch(),
                          .last_read_at = core::Time::epoch(),
                          .importance = 0.75,
                          .tags = {"secret", "project"},
                          .linked_record_ids = {"linked-1"},
                          .shadow = false,
                      },
                  .score = 0.9,
                  .lexical_score = 0.8,
                  .redacted_record =
                      hook::RedactedMemoryRecordPayload{
                          .id = "memory-1",
                          .scope_key = "scope",
                          .kind = "project",
                          .title_bytes = std::string_view{"Sensitive title"}.size(),
                          .body_bytes = std::string_view{"Sensitive body"}.size(),
                          .tag_count = 2,
                          .linked_record_count = 1,
                          .shadow = false,
                      },
              },
          },
      .hybrid = false,
      .started_at = core::Time::epoch(),
      .finished_at = core::Time::epoch(),
  };
}

hook::MemoryDecayPayload sample_memory_decay() {
  return hook::MemoryDecayPayload{
      .who = hook::Identity{.scope_key = "cli", .agent_key = "bootstrap", .identity = "startup"},
      .source = "startup",
      .scope_key = "cli",
      .unused_before = core::Time{core::Time::time_point{10s}},
      .importance_floor = 0.5,
      .limit = 10,
      .decay_at = core::Time{core::Time::time_point{30s}},
      .shadowed_count = 3,
      .started_at = core::Time::epoch(),
      .finished_at = core::Time{core::Time::time_point{1ms}},
      .duration = 1ms,
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

TEST_CASE("publish_advisory delivers memory decay metadata", "[hook][bus][memory]") {
  hook::Bus bus;
  hook::MemoryDecayPayload captured;
  hook::InProcessSink sink{"decay-recorder",
                           [&](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
                             REQUIRE(event == hook::Event::memory_decay);
                             const auto* decay = std::get_if<hook::MemoryDecayPayload>(payload.get());
                             REQUIRE(decay != nullptr);
                             captured = *decay;
                             co_return core::Result<void>{};
                           }};
  bus.bind(sink, {hook::Event::memory_decay});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto outcome = co_await bus.publish_advisory(hook::Event::memory_decay, sample_memory_decay());
    REQUIRE(outcome.sinks.size() == 1);
    REQUIRE(outcome.sinks[0].sink_id == "decay-recorder");
    REQUIRE(outcome.all_succeeded());
    co_return;
  });

  REQUIRE(captured.source == "startup");
  REQUIRE(captured.scope_key == "cli");
  REQUIRE(captured.importance_floor == 0.5);
  REQUIRE(captured.limit == 10);
  REQUIRE(captured.shadowed_count == 3);
  REQUIRE(captured.duration == 1ms);
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

TEST_CASE("publish_advisory fans out async sinks while preserving outcome order", "[hook][bus]") {
  hook::Bus bus;
  std::size_t active = 0;
  std::size_t peak_active = 0;
  std::vector<std::string> completions;
  DelayedSink first{"first", 40ms, active, peak_active, completions};
  DelayedSink second{"second", 10ms, active, peak_active, completions};
  DelayedSink third{"third", 20ms, active, peak_active, completions};
  bus.bind(first, {hook::Event::tool_before});
  bus.bind(second, {hook::Event::tool_before});
  bus.bind(third, {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto outcome = co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
    REQUIRE(outcome.sinks.size() == 3);
    REQUIRE(outcome.sinks[0].sink_id == "first");
    REQUIRE(outcome.sinks[1].sink_id == "second");
    REQUIRE(outcome.sinks[2].sink_id == "third");
    REQUIRE(outcome.all_succeeded());
    co_return;
  });

  REQUIRE(peak_active == 3);
  REQUIRE(completions.size() == 3);
  REQUIRE(completions.front() == "second");
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

TEST_CASE("publish_advisory redacts input_json when a sanitized view is present", "[hook][bus][redaction]") {
  hook::Bus bus;
  RecordingSink default_sink{"default"};
  RecordingSink trusted_sink{"trusted", hook::SinkKind::trusted_local};
  bus.bind(default_sink, {hook::Event::tool_after});
  bus.bind(trusted_sink, {hook::Event::tool_after});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto outcome = co_await bus.publish_advisory(hook::Event::tool_after, sample_after_with_redacted_input());
    REQUIRE(outcome.sinks.size() == 2);
    REQUIRE(outcome.all_succeeded());
    co_return;
  });

  REQUIRE(default_sink.captures().size() == 1);
  REQUIRE(default_sink.captures()[0].input_json ==
          R"({"kind":"redacted_tool_input","input_hash":"abc","content_bytes":6})");

  REQUIRE(trusted_sink.captures().size() == 1);
  REQUIRE(trusted_sink.captures()[0].input_json == R"({"path":"notes.md","content":"secret"})");
}

TEST_CASE("publish_advisory redacts memory read query and records for default sinks",
          "[hook][bus][redaction][memory]") {
  hook::Bus bus;
  hook::MemoryReadPayload default_payload;
  hook::MemoryReadPayload trusted_payload;
  hook::InProcessSink default_sink{
      "default",
      [&](hook::Event /*event*/, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
        const auto* read = std::get_if<hook::MemoryReadPayload>(payload.get());
        REQUIRE(read != nullptr);
        default_payload = *read;
        co_return core::Result<void>{};
      }};
  hook::InProcessSink trusted_sink{
      "trusted",
      [&](hook::Event /*event*/, hook::PayloadPtr payload) -> async::Awaitable<core::Result<void>> {
        const auto* read = std::get_if<hook::MemoryReadPayload>(payload.get());
        REQUIRE(read != nullptr);
        trusted_payload = *read;
        co_return core::Result<void>{};
      },
      hook::SinkKind::trusted_local};
  bus.bind(default_sink, {hook::Event::memory_read_after});
  bus.bind(trusted_sink, {hook::Event::memory_read_after});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto outcome = co_await bus.publish_advisory(hook::Event::memory_read_after, sample_memory_read());
    REQUIRE(outcome.sinks.size() == 2);
    REQUIRE(outcome.all_succeeded());
    co_return;
  });

  REQUIRE(default_payload.source == "memory.recall");
  REQUIRE(default_payload.query.empty());
  REQUIRE(default_payload.redacted_query_bytes == std::string_view{"sensitive query"}.size());
  REQUIRE(default_payload.match_count == 1);
  REQUIRE(default_payload.hits.size() == 1);
  REQUIRE(default_payload.hits[0].record.id == "memory-1");
  REQUIRE(default_payload.hits[0].record.title.empty());
  REQUIRE(default_payload.hits[0].record.body.empty());
  REQUIRE(default_payload.hits[0].record.tags.empty());
  REQUIRE(default_payload.hits[0].record.linked_record_ids.empty());
  REQUIRE(default_payload.hits[0].redacted_record.has_value());
  REQUIRE(default_payload.hits[0].redacted_record->body_bytes == std::string_view{"Sensitive body"}.size());

  REQUIRE(trusted_payload.query == "sensitive query");
  REQUIRE(trusted_payload.hits.size() == 1);
  REQUIRE(trusted_payload.hits[0].record.title == "Sensitive title");
  REQUIRE(trusted_payload.hits[0].record.body == "Sensitive body");
  REQUIRE(trusted_payload.hits[0].record.tags == std::vector<std::string>{"secret", "project"});
  REQUIRE(trusted_payload.hits[0].record.linked_record_ids == std::vector<std::string>{"linked-1"});
}

TEST_CASE("publish_advisory shares redacted payload snapshots across default sinks", "[hook][bus][redaction]") {
  hook::Bus bus;
  std::vector<const hook::Payload*> default_payloads;
  std::vector<const hook::Payload*> trusted_payloads;
  std::vector<std::string> default_inputs;
  std::vector<std::string> trusted_inputs;
  PayloadPointerSink first_default{"default-1", default_payloads, default_inputs};
  PayloadPointerSink second_default{"default-2", default_payloads, default_inputs};
  PayloadPointerSink trusted{"trusted", trusted_payloads, trusted_inputs, hook::SinkKind::trusted_local};
  bus.bind(first_default, {hook::Event::tool_after});
  bus.bind(second_default, {hook::Event::tool_after});
  bus.bind(trusted, {hook::Event::tool_after});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    auto outcome = co_await bus.publish_advisory(hook::Event::tool_after, sample_after_with_redacted_input());
    REQUIRE(outcome.sinks.size() == 3);
    REQUIRE(outcome.all_succeeded());
    co_return;
  });

  REQUIRE(default_payloads.size() == 2);
  REQUIRE(default_payloads[0] == default_payloads[1]);
  REQUIRE(trusted_payloads.size() == 1);
  REQUIRE(default_payloads[0] != trusted_payloads[0]);
  REQUIRE(default_inputs.size() == 2);
  REQUIRE(default_inputs[0] == R"({"kind":"redacted_tool_input","input_hash":"abc","content_bytes":6})");
  REQUIRE(default_inputs[1] == default_inputs[0]);
  REQUIRE(trusted_inputs.size() == 1);
  REQUIRE(trusted_inputs[0] == R"({"path":"notes.md","content":"secret"})");
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

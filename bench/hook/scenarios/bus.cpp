// bench/hook/scenarios/bus.cpp
//
// Slice 22 — `hook::Bus::publish_advisory` overhead at varying fan-out.
// Slice 156 — advisory publishes start subscribed sinks as sibling
// coroutines, so the no-op scenarios now measure fan-out/gather overhead
// rather than a sequential await loop.
// Slice 91 — matching `publish_blocking<Event::tool_before>` overhead so
// spec 0015 can compare the blocking path against the advisory baseline.
// Slice 158 — large redacted `tool_after` scenarios pin the shared immutable
// payload snapshots that keep multi-sink fan-out from cloning structured bytes
// once per subscribed default sink.
//
// A/B/C scenarios:
//
//   A. `publish_no_sinks` — bus has no sinks subscribed to `tool_before`. The
//      publish should be near-free (one map lookup + early return).
//   B. `publish_one_sink` — bus has one InProcessSink subscribed. The sink's
//      callback is a noop (assigns a side-effect counter to defeat dead-code
//      elimination). Measures the per-sink dispatch overhead.
//   C. `publish_three_sinks` — three sinks subscribed. Measures whether the
//      per-sink overhead is roughly linear in subscriber count.
//
// The contrast (B - A) is the per-sink dispatch cost the bus pays; (C - A) is
// the cost of three sinks. The baseline a future "batch publish" or "lock-free
// sink list" optimisation would need to beat.

#include <nanobench.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/hook.hpp>

namespace orangutan::bench {

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace hook = orangutan::hook;

namespace {

[[gnu::noinline]] std::size_t
drive_publish(asio::io_context& io, hook::Bus& bus, hook::Event event, hook::Payload& payload) {
  std::size_t counter = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto outcome = co_await bus.publish_advisory(event, payload);
        counter += outcome.sinks.size();
        co_return;
      },
      asio::detached);
  io.run();
  io.restart();
  return counter;
}

[[gnu::noinline]] std::size_t drive_blocking_publish(asio::io_context& io, hook::Bus& bus, hook::Payload& payload) {
  std::size_t counter = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto decision = co_await bus.publish_blocking<hook::Event::tool_before>(payload);
        if (decision.has_value()) {
          counter += decision->trace.size();
          counter += static_cast<std::size_t>(decision->kind);
        }
        co_return;
      },
      asio::detached);
  io.run();
  io.restart();
  return counter;
}

hook::InProcessSink make_noop_sink(std::string id, std::size_t& counter) {
  return hook::InProcessSink{std::move(id),
                             [&counter](hook::Event, hook::PayloadPtr) -> async::Awaitable<core::Result<void>> {
                               counter++;
                               co_return core::Result<void>{};
                             }};
}

class BlockingBenchSink final : public hook::Sink {
public:
  BlockingBenchSink(std::string id, std::size_t& counter, hook::HookDecision decision = {})
      : id_(std::move(id)), counter_(&counter), decision_(std::move(decision)) {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return id_;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> receive(hook::Event, hook::PayloadPtr) override {
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<hook::HookDecision>> handle_blocking(hook::Event,
                                                                                   hook::PayloadPtr) override {
    ++(*counter_);
    co_return decision_;
  }

private:
  std::string id_;
  std::size_t* counter_;
  hook::HookDecision decision_;
};

hook::Payload sample_payload() {
  return hook::Payload{hook::ToolBeforePayload{
      .tool_name = "noop",
      .input_json = "{}",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .started_at = core::Time::epoch(),
  }};
}

hook::Payload large_redacted_after_payload() {
  return hook::Payload{hook::ToolAfterPayload{
      .tool_name = "FileWrite",
      .input_json = std::string(16 * 1024, 'i'),
      .redacted_input_json = R"({"kind":"redacted_tool_input","input_hash":"bench","content_bytes":16384})",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .succeeded = true,
      .output_text = std::string(8 * 1024, 'o'),
      .data_json = std::string(64 * 1024, 'd'),
      .error_kind = "",
      .error_message = "",
      .started_at = core::Time::epoch(),
      .finished_at = core::Time::epoch(),
  }};
}

}  // namespace

void register_hook_bus(ankerl::nanobench::Bench& bench) {
  asio::io_context io;
  auto payload_a = sample_payload();
  auto payload_b = sample_payload();
  auto payload_c = sample_payload();

  hook::Bus bus_a;
  bench.run("publish_no_sinks", [&] {
    const auto value = drive_publish(io, bus_a, hook::Event::tool_before, payload_a);
    ankerl::nanobench::doNotOptimizeAway(value);
  });

  hook::Bus bus_b;
  std::size_t counter_b = 0;
  auto sink_b = make_noop_sink("sink-b", counter_b);
  bus_b.bind(sink_b, {hook::Event::tool_before});
  bench.run("publish_one_sink", [&] {
    const auto value = drive_publish(io, bus_b, hook::Event::tool_before, payload_b);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  ankerl::nanobench::doNotOptimizeAway(counter_b);

  hook::Bus bus_c;
  std::size_t counter_c1 = 0;
  std::size_t counter_c2 = 0;
  std::size_t counter_c3 = 0;
  auto sink_c1 = make_noop_sink("sink-c1", counter_c1);
  auto sink_c2 = make_noop_sink("sink-c2", counter_c2);
  auto sink_c3 = make_noop_sink("sink-c3", counter_c3);
  bus_c.bind(sink_c1, {hook::Event::tool_before});
  bus_c.bind(sink_c2, {hook::Event::tool_before});
  bus_c.bind(sink_c3, {hook::Event::tool_before});
  bench.run("publish_three_sinks", [&] {
    const auto value = drive_publish(io, bus_c, hook::Event::tool_before, payload_c);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  ankerl::nanobench::doNotOptimizeAway(counter_c1);
  ankerl::nanobench::doNotOptimizeAway(counter_c2);
  ankerl::nanobench::doNotOptimizeAway(counter_c3);

  auto payload_d = sample_payload();
  hook::Bus bus_d;
  bench.run("publish_blocking_no_sinks", [&] {
    const auto value = drive_blocking_publish(io, bus_d, payload_d);
    ankerl::nanobench::doNotOptimizeAway(value);
  });

  auto payload_e = sample_payload();
  hook::Bus bus_e;
  std::size_t counter_e = 0;
  BlockingBenchSink sink_e{"sink-e", counter_e};
  bus_e.bind(sink_e, {hook::Event::tool_before});
  bench.run("publish_blocking_one_sink", [&] {
    const auto value = drive_blocking_publish(io, bus_e, payload_e);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  ankerl::nanobench::doNotOptimizeAway(counter_e);

  auto payload_f = sample_payload();
  hook::Bus bus_f;
  std::size_t counter_f1 = 0;
  std::size_t counter_f2 = 0;
  std::size_t counter_f3 = 0;
  BlockingBenchSink sink_f1{"sink-f1", counter_f1};
  BlockingBenchSink sink_f2{"sink-f2", counter_f2};
  BlockingBenchSink sink_f3{"sink-f3", counter_f3};
  bus_f.bind(sink_f1, {hook::Event::tool_before});
  bus_f.bind(sink_f2, {hook::Event::tool_before});
  bus_f.bind(sink_f3, {hook::Event::tool_before});
  bench.run("publish_blocking_three_sinks_all_proceed", [&] {
    const auto value = drive_blocking_publish(io, bus_f, payload_f);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  ankerl::nanobench::doNotOptimizeAway(counter_f1);
  ankerl::nanobench::doNotOptimizeAway(counter_f2);
  ankerl::nanobench::doNotOptimizeAway(counter_f3);

  auto payload_g = sample_payload();
  hook::Bus bus_g;
  std::size_t counter_g1 = 0;
  std::size_t counter_g2 = 0;
  std::size_t counter_g3 = 0;
  hook::HookDecision veto{};
  veto.kind = hook::HookDecisionKind::veto;
  veto.reason = "bench";
  BlockingBenchSink sink_g1{"sink-g1", counter_g1};
  BlockingBenchSink sink_g2{"sink-g2", counter_g2, std::move(veto)};
  BlockingBenchSink sink_g3{"sink-g3", counter_g3};
  bus_g.bind(sink_g1, {hook::Event::tool_before});
  bus_g.bind(sink_g2, {hook::Event::tool_before});
  bus_g.bind(sink_g3, {hook::Event::tool_before});
  bench.run("publish_blocking_short_circuit_second", [&] {
    const auto value = drive_blocking_publish(io, bus_g, payload_g);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  ankerl::nanobench::doNotOptimizeAway(counter_g1);
  ankerl::nanobench::doNotOptimizeAway(counter_g2);
  ankerl::nanobench::doNotOptimizeAway(counter_g3);

  auto payload_h = large_redacted_after_payload();
  hook::Bus bus_h;
  std::size_t counter_h = 0;
  auto sink_h = make_noop_sink("sink-h", counter_h);
  bus_h.bind(sink_h, {hook::Event::tool_after});
  bench.run("publish_one_default_sink_large_redacted_payload", [&] {
    const auto value = drive_publish(io, bus_h, hook::Event::tool_after, payload_h);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  ankerl::nanobench::doNotOptimizeAway(counter_h);

  auto payload_i = large_redacted_after_payload();
  hook::Bus bus_i;
  std::size_t counter_i1 = 0;
  std::size_t counter_i2 = 0;
  std::size_t counter_i3 = 0;
  auto sink_i1 = make_noop_sink("sink-i1", counter_i1);
  auto sink_i2 = make_noop_sink("sink-i2", counter_i2);
  auto sink_i3 = make_noop_sink("sink-i3", counter_i3);
  bus_i.bind(sink_i1, {hook::Event::tool_after});
  bus_i.bind(sink_i2, {hook::Event::tool_after});
  bus_i.bind(sink_i3, {hook::Event::tool_after});
  bench.run("publish_three_default_sinks_large_redacted_payload", [&] {
    const auto value = drive_publish(io, bus_i, hook::Event::tool_after, payload_i);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  ankerl::nanobench::doNotOptimizeAway(counter_i1);
  ankerl::nanobench::doNotOptimizeAway(counter_i2);
  ankerl::nanobench::doNotOptimizeAway(counter_i3);
}

}  // namespace orangutan::bench

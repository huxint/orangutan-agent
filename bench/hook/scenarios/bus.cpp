// bench/hook/scenarios/bus.cpp
//
// Slice 22 — `hook::Bus::publish_advisory` overhead at varying fan-out.
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

[[gnu::noinline]] std::size_t drive_publish(asio::io_context& io, hook::Bus& bus, hook::ToolBeforePayload& payload) {
  std::size_t counter = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto outcome = co_await bus.publish_advisory(hook::Event::tool_before, payload);
        counter += outcome.sinks.size();
        co_return;
      },
      asio::detached);
  io.run();
  io.restart();
  return counter;
}

hook::InProcessSink make_noop_sink(std::string id, std::size_t& counter) {
  return hook::InProcessSink{std::move(id),
                             [&counter](hook::Event, hook::Payload) -> async::Awaitable<core::Result<void>> {
                               counter++;
                               co_return core::Result<void>{};
                             }};
}

hook::ToolBeforePayload sample_payload() {
  return hook::ToolBeforePayload{
      .tool_name = "noop",
      .input_json = "{}",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .started_at = core::Time::epoch(),
  };
}

}  // namespace

void register_hook_bus(ankerl::nanobench::Bench& bench) {
  asio::io_context io;
  auto payload_a = sample_payload();
  auto payload_b = sample_payload();
  auto payload_c = sample_payload();

  hook::Bus bus_a;
  bench.run("publish_no_sinks", [&] {
    const auto value = drive_publish(io, bus_a, payload_a);
    ankerl::nanobench::doNotOptimizeAway(value);
  });

  hook::Bus bus_b;
  std::size_t counter_b = 0;
  auto sink_b = make_noop_sink("sink-b", counter_b);
  bus_b.bind(sink_b, {hook::Event::tool_before});
  bench.run("publish_one_sink", [&] {
    const auto value = drive_publish(io, bus_b, payload_b);
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
    const auto value = drive_publish(io, bus_c, payload_c);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  ankerl::nanobench::doNotOptimizeAway(counter_c1);
  ankerl::nanobench::doNotOptimizeAway(counter_c2);
  ankerl::nanobench::doNotOptimizeAway(counter_c3);
}

}  // namespace orangutan::bench

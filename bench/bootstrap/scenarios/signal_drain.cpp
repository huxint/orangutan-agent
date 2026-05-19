// bench/bootstrap/scenarios/signal_drain.cpp
//
// A-vs-B: cost of `SignalScope` vs. a bare `io_context` drain over the
// same workload. The win shape here is "what does installing the
// SIGINT/SIGTERM trap add per `--audit-init`-class drain?" — useful as a
// baseline once long-running flows (agent loop, web server) inherit the
// scope.

#include <nanobench.h>

#include <cstddef>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <oran/bootstrap/signal_drain.hpp>

namespace orangutan::bench {
namespace bootstrap = orangutan::bootstrap;

namespace {

constexpr std::size_t kWorkItems = 8;

[[gnu::noinline]] std::size_t drain_with_scope() {
  asio::io_context io;
  bootstrap::SignalScope scope{io};
  std::size_t hits = 0;
  for (std::size_t i = 0; i < kWorkItems; ++i) {
    asio::post(io, [&hits] { ++hits; });
  }
  asio::post(io, [&] { scope.release(); });
  io.run();
  return hits;
}

[[gnu::noinline]] std::size_t drain_bare() {
  asio::io_context io;
  std::size_t hits = 0;
  for (std::size_t i = 0; i < kWorkItems; ++i) {
    asio::post(io, [&hits] { ++hits; });
  }
  io.run();
  return hits;
}

}  // namespace

void register_signal_drain(ankerl::nanobench::Bench& bench) {
  // The drain is µs-scale; nanobench's default warmup is enough.
  bench.minEpochIterations(50000);

  bench.run("bootstrap.signal_drain_with_scope", [&] {
    const auto v = drain_with_scope();
    ankerl::nanobench::doNotOptimizeAway(v);
  });
  bench.run("bootstrap.signal_drain_bare", [&] {
    const auto v = drain_bare();
    ankerl::nanobench::doNotOptimizeAway(v);
  });
}

}  // namespace orangutan::bench

// bench/channel/scenarios/mock_ingress.cpp
//
// A-vs-B comparison: direct prompt-runner invocation on a fabricated request
// vs. the full mock ingress boundary (MockChannel push -> manager fan-in ->
// dispatch_one -> reply send). This prices the channel ingress seam that
// bootstrap routing will sit on before concrete platform adapters land.

#include <nanobench.h>

#include <expected>
#include <memory>
#include <string>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/channel.hpp>
#include <oran/core/content.hpp>

namespace orangutan::bench {

namespace {

constexpr int kMessages = 16;

[[nodiscard]] channel::InboundMessage bench_inbound() {
  return channel::InboundMessage{
      .channel_id = {},
      .conversation_id = "bench-room",
      .user_id = "bench-user",
      .display_name = "Bench User",
      .content = {core::TextContent{.text = "hello"}},
      .replies_to = {},
      .received_at = core::Time::epoch(),
  };
}

[[nodiscard]] channel::ChannelPromptRunner echo_runner() {
  return
      [](channel::ChannelPromptRunRequest request) -> async::Awaitable<core::Result<channel::ChannelPromptRunResult>> {
        co_return channel::ChannelPromptRunResult{.text = std::move(request.prompt)};
      };
}

[[gnu::noinline]] std::size_t run_direct_runner() {
  asio::io_context io;
  std::size_t replied{};
  auto runner = echo_runner();

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        for (int i = 0; i < kMessages; ++i) {
          auto message = bench_inbound();
          auto request = channel::make_prompt_run_request(message);
          if (!request) {
            break;
          }
          auto result = co_await runner(std::move(*request));
          if (!result) {
            break;
          }
          auto reply = channel::make_reply_message(message, std::move(result->text));
          replied += reply.content.size();
        }
        io.stop();
        co_return;
      },
      asio::detached);

  io.run();
  return replied;
}

[[gnu::noinline]] std::size_t run_mock_ingress_dispatch() {
  asio::io_context io;
  std::size_t replied{};
  auto runner = echo_runner();

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto adapter = std::make_unique<channel::MockChannel>(
            io.get_executor(),
            channel::MockChannelOptions{.id = "bench-mock", .inbound_capacity = 32});
        auto* mock = adapter.get();
        channel::ChannelManager manager{io.get_executor(), channel::ChannelManagerOptions{.inbound_capacity = 32}};
        if (!manager.register_adapter(std::move(adapter))) {
          io.stop();
          co_return;
        }
        auto started = co_await manager.start_all();
        if (!started) {
          io.stop();
          co_return;
        }
        for (int i = 0; i < kMessages; ++i) {
          if (!mock->push_inbound(bench_inbound())) {
            break;
          }
          auto pumped = co_await manager.receive_one("bench-mock");
          if (!pumped) {
            break;
          }
          auto receipt = co_await channel::dispatch_one(manager, runner);
          if (!receipt) {
            break;
          }
          ++replied;
        }
        io.stop();
        co_return;
      },
      asio::detached);

  io.run();
  return replied;
}

}  // namespace

void register_channel_mock_ingress(ankerl::nanobench::Bench& bench) {
  bench.run("channel.direct_runner", [] {
    auto value = run_direct_runner();
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("channel.mock_ingress_dispatch", [] {
    auto value = run_mock_ingress_dispatch();
    ankerl::nanobench::doNotOptimizeAway(value);
  });
}

}  // namespace orangutan::bench

// tests/async/test_async.cpp — Runtime, sleep_for, and Channel coverage.

#include <atomic>
#include <chrono>
#include <exception>
#include <expected>
#include <optional>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>

#include "../test-helpers/run_async.hpp"

using namespace std::chrono_literals;

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace test = orangutan::tests;

TEST_CASE("Runtime runs work on its executor until stopped", "[unit][async][runtime]") {
  async::Runtime runtime{async::RuntimeConfig{.io_workers = 1, .cpu_workers = 1}};
  std::atomic_bool ran = false;

  asio::co_spawn(
      runtime.executor(),
      [&]() -> async::Awaitable<void> {
        ran = true;
        runtime.stop();
        co_return;
      },
      asio::detached);

  auto result = runtime.run();
  REQUIRE(result.has_value());
  REQUIRE(ran.load());
}

TEST_CASE("sleep_for completes on timer expiry", "[unit][async][sleep]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto result = co_await async::sleep_for(io.get_executor(), 1ms);
    REQUIRE(result.has_value());
  });
}

TEST_CASE("sleep_for reports cancellation", "[unit][async][sleep]") {
  asio::io_context io;
  asio::cancellation_signal signal;
  std::optional<core::Result<void>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<void>> { co_return co_await async::sleep_for(io.get_executor(), 1s); },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<void> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
}

TEST_CASE("Channel sends and receives FIFO values", "[unit][async][channel]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    async::Channel<int> channel{io.get_executor(), 2};

    auto sent_first = co_await channel.send(1);
    auto sent_second = co_await channel.send(2);
    REQUIRE(sent_first.has_value());
    REQUIRE(sent_second.has_value());

    auto first = co_await channel.receive();
    auto second = co_await channel.receive();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(*first == 1);
    REQUIRE(*second == 2);
  });
}

TEST_CASE("Channel try_send reports overflow when full", "[unit][async][channel]") {
  asio::io_context io;
  async::Channel<int> channel{io.get_executor(), 1};

  REQUIRE(channel.try_send(7).has_value());

  auto overflow = channel.try_send(8);
  REQUIRE_FALSE(overflow.has_value());
  REQUIRE(overflow.error().kind() == core::ErrorKind::mailbox_overflowed);
}

TEST_CASE("Channel pending send completes when receive frees capacity", "[unit][async][channel]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    async::Channel<int> channel{io.get_executor(), 1};
    REQUIRE(channel.try_send(1).has_value());

    std::optional<core::Result<void>> send_result;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<void> {
          send_result = co_await channel.send(2);
          co_return;
        },
        asio::detached);

    co_await asio::post(io, asio::use_awaitable);
    REQUIRE_FALSE(send_result.has_value());

    auto first = co_await channel.receive();
    REQUIRE(first.has_value());
    REQUIRE(*first == 1);

    co_await asio::post(io, asio::use_awaitable);
    REQUIRE(send_result.has_value());
    REQUIRE(send_result->has_value());

    auto second = co_await channel.receive();
    REQUIRE(second.has_value());
    REQUIRE(*second == 2);
  });
}

TEST_CASE("Channel close drains buffered values then reports closed", "[unit][async][channel]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    async::Channel<int> channel{io.get_executor(), 2};

    REQUIRE(channel.try_send(1).has_value());
    REQUIRE(channel.try_send(2).has_value());
    channel.close();

    auto first = co_await channel.receive();
    auto second = co_await channel.receive();
    auto closed = co_await channel.receive();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(*first == 1);
    REQUIRE(*second == 2);
    REQUIRE_FALSE(closed.has_value());
    REQUIRE(closed.error().kind() == core::ErrorKind::cancelled);
  });
}

TEST_CASE("Channel receive observes coroutine cancellation", "[unit][async][channel]") {
  asio::io_context io;
  async::Channel<int> channel{io.get_executor(), 0};
  asio::cancellation_signal signal;
  std::optional<core::Result<int>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<int>> { co_return co_await channel.receive(); },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<int> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
}

// Slice 31 regression: a Channel created on executor A but driven by a
// coroutine spawned on strand B must resume the coroutine on strand B.
// Before the fix, completions were posted to the channel's executor (A),
// which would race against any other work on B and quietly violate the
// strand's serialization guarantee.
TEST_CASE("Channel resumes coroutine on its associated executor (not the channel's)",
          "[unit][async][channel][regression]") {
  asio::thread_pool pool{2};
  auto strand_a = asio::make_strand(pool);
  auto strand_b = asio::make_strand(pool);

  async::Channel<int> channel{strand_a, 1};

  std::atomic_bool ran_on_strand_b = false;
  std::atomic_bool finished = false;

  asio::co_spawn(
      strand_b,
      [&]() -> async::Awaitable<void> {
        // Ask asio for the executor the continuation will resume on. With
        // the slice-31 fix, this is strand_b; before the fix, the channel
        // ignored the handler's associated executor and posted to strand_a.
        auto received = co_await channel.receive();
        REQUIRE(received.has_value());
        REQUIRE(*received == 7);
        // running_in_this_thread() is the cheapest cross-executor check —
        // when on strand_b's thread, only strand_b's posted work runs.
        ran_on_strand_b = strand_b.running_in_this_thread();
        finished = true;
        co_return;
      },
      asio::detached);

  // Send via the channel's executor; this enqueues the value and triggers
  // pump_locked, which would have posted the receiver's completion to
  // strand_a under the old code.
  asio::post(strand_a, [&] { REQUIRE(channel.try_send(7).has_value()); });

  pool.join();

  REQUIRE(finished.load());
  REQUIRE(ran_on_strand_b.load());
}

// tests/async/test_async.cpp — Runtime, sleep_for, and Channel coverage.

#include <atomic>
#include <chrono>
#include <exception>
#include <expected>
#include <optional>
#include <stdexcept>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>

using namespace std::chrono_literals;

namespace async = orangutan::async;
namespace core = orangutan::core;

namespace {

template <typename Fn>
void run_async(Fn&& fn) {
  asio::io_context io;
  std::exception_ptr failure;
  bool finished = false;

  asio::steady_timer timeout{io};
  timeout.expires_after(1s);
  timeout.async_wait([&](const asio::error_code& ec) {
    if (!ec && !finished) {
      failure = std::make_exception_ptr(std::runtime_error{"async test timed out"});
      io.stop();
    }
  });

  asio::co_spawn(io, std::forward<Fn>(fn)(io), [&](std::exception_ptr ep) {
    finished = true;
    if (ep) {
      failure = ep;
    }
    timeout.cancel();
    io.stop();
  });

  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(finished);
}

}  // namespace

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
  run_async([](asio::io_context& io) -> async::Awaitable<void> {
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
  run_async([](asio::io_context& io) -> async::Awaitable<void> {
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
  run_async([](asio::io_context& io) -> async::Awaitable<void> {
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
  run_async([](asio::io_context& io) -> async::Awaitable<void> {
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

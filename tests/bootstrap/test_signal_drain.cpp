// tests/bootstrap/test_signal_drain.cpp — signal-aware io_context drain coverage.

#include <chrono>
#include <csignal>
#include <string>

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/bootstrap/signal_drain.hpp>
#include <oran/core/error.hpp>

namespace bootstrap = orangutan::bootstrap;
namespace core = orangutan::core;

TEST_CASE("SignalScope::release lets io.run() return when no work remains", "[unit][bootstrap][signal_drain]") {
  asio::io_context io;
  bootstrap::SignalScope scope{io};
  scope.release();
  io.run();
  REQUIRE(scope.signum() == 0);
}

TEST_CASE("SignalScope runs pending work to completion when released after work", "[unit][bootstrap][signal_drain]") {
  asio::io_context io;
  bootstrap::SignalScope scope{io};

  int hits = 0;
  for (int i = 0; i < 4; ++i) {
    asio::post(io, [&hits] { ++hits; });
  }
  asio::post(io, [&] { scope.release(); });

  io.run();
  REQUIRE(scope.signum() == 0);
  REQUIRE(hits == 4);
}

TEST_CASE("SignalScope captures SIGTERM and stops io.run()", "[unit][bootstrap][signal_drain]") {
  asio::io_context io;
  bootstrap::SignalScope scope{io};

  // A long timer guarantees io has more work than just the signal_set,
  // so the signal-driven `io.stop()` (not natural drain) is the
  // termination cause.
  asio::steady_timer keepalive{io};
  keepalive.expires_after(std::chrono::seconds{10});
  keepalive.async_wait([](const asio::error_code&) {});

  asio::post(io, [] { ::raise(SIGTERM); });

  io.run();
  REQUIRE(scope.signum() == SIGTERM);
}

TEST_CASE("SignalScope captures SIGINT and stops io.run()", "[unit][bootstrap][signal_drain]") {
  asio::io_context io;
  bootstrap::SignalScope scope{io};

  asio::steady_timer keepalive{io};
  keepalive.expires_after(std::chrono::seconds{10});
  keepalive.async_wait([](const asio::error_code&) {});

  asio::post(io, [] { ::raise(SIGINT); });

  io.run();
  REQUIRE(scope.signum() == SIGINT);
}

TEST_CASE("SignalScope::release is idempotent", "[unit][bootstrap][signal_drain]") {
  asio::io_context io;
  bootstrap::SignalScope scope{io};
  scope.release();
  scope.release();
  io.run();
  REQUIRE(scope.signum() == 0);
}

TEST_CASE("signum_from_error recovers the signum from a cancelled error", "[unit][bootstrap][signal_drain]") {
  auto error = core::Error::cancelled().with("signal", "SIGTERM").with("signum", "15");
  const auto signum = bootstrap::signum_from_error(error);
  REQUIRE(signum.has_value());
  REQUIRE(*signum == 15);
}

TEST_CASE("signum_from_error rejects non-cancelled errors", "[unit][bootstrap][signal_drain]") {
  auto error = core::Error::io("not cancelled").with("signum", "15");
  REQUIRE_FALSE(bootstrap::signum_from_error(error).has_value());
}

TEST_CASE("signum_from_error rejects missing signum context", "[unit][bootstrap][signal_drain]") {
  auto error = core::Error::cancelled().with("signal", "SIGTERM");
  REQUIRE_FALSE(bootstrap::signum_from_error(error).has_value());
}

TEST_CASE("signum_from_error rejects malformed signum context", "[unit][bootstrap][signal_drain]") {
  auto malformed = core::Error::cancelled().with("signum", "abc");
  REQUIRE_FALSE(bootstrap::signum_from_error(malformed).has_value());

  auto negative = core::Error::cancelled().with("signum", "-1");
  REQUIRE_FALSE(bootstrap::signum_from_error(negative).has_value());

  auto trailing = core::Error::cancelled().with("signum", "15x");
  REQUIRE_FALSE(bootstrap::signum_from_error(trailing).has_value());
}

TEST_CASE("signal_name maps the supported signals", "[unit][bootstrap][signal_drain]") {
  REQUIRE(std::string{bootstrap::signal_name(SIGINT)} == "SIGINT");
  REQUIRE(std::string{bootstrap::signal_name(SIGTERM)} == "SIGTERM");
  REQUIRE(std::string{bootstrap::signal_name(0)} == "unknown");
  REQUIRE(std::string{bootstrap::signal_name(99)} == "unknown");
}

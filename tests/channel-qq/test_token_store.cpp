// tests/channel-qq/test_token_store.cpp — QQ app-access-token store coverage.

#include <oran/channel-qq/token_store.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/http/client.hpp>

#include "../test-helpers/run_async.hpp"
#include "scripted_http_server.hpp"

using namespace std::chrono_literals;

namespace {

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace http = orangutan::http;
namespace qq = orangutan::channel::qq;
namespace test = orangutan::tests;

[[nodiscard]] qq::Credentials test_credentials() {
  return qq::Credentials{.app_id = "test-app", .client_secret = "test-secret"};
}

[[nodiscard]] std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  for (const auto& [name, value] : error.context()) {
    if (name == key) {
      return value;
    }
  }
  return std::nullopt;
}

[[nodiscard]] test::ScriptedResponse
token_ok(std::string token, std::string expires_in = "7200", std::chrono::milliseconds delay = 0ms) {
  return test::ScriptedResponse{
      .status = 200,
      .body = R"({"access_token":")" + std::move(token) + R"(","expires_in":)" + std::move(expires_in) + "}",
      .headers = {{"Content-Type", "application/json"}},
      .delay = delay,
  };
}

[[nodiscard]] qq::TokenStoreOptions store_options(const test::ScriptedHttpServer& server) {
  auto options = qq::TokenStoreOptions{};
  options.token_url = server.url("/app/getAppAccessToken");
  options.request_timeout = 2s;
  return options;
}

}  // namespace

TEST_CASE("token store fetches and caches the app access token", "[unit][channel-qq][token]") {
  test::ScriptedHttpServer server{{token_ok("tok-1")}};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};
  auto store = qq::TokenStore{client, test_credentials(), store_options(server)};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    const auto now = std::chrono::steady_clock::now();

    auto first = co_await store.access_token(now);
    REQUIRE(first.has_value());
    REQUIRE(*first == "tok-1");

    auto second = co_await store.access_token(now + 1s);
    REQUIRE(second.has_value());
    REQUIRE(*second == "tok-1");
  });

  REQUIRE(server.served_count() == 1);
  REQUIRE(store.refresh_count() == 1);
  const auto request = server.request_text(0);
  REQUIRE(request.starts_with("POST /app/getAppAccessToken HTTP/1.1"));
  REQUIRE(request.contains("Content-Type: application/json"));
  REQUIRE(request.contains(R"("appId":"test-app")"));
  REQUIRE(request.contains(R"("clientSecret":"test-secret")"));
  blocking.join();
}

TEST_CASE("token store refreshes inside the refresh-ahead window", "[unit][channel-qq][token]") {
  test::ScriptedHttpServer server{{token_ok("tok-1"), token_ok("tok-2")}};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};
  auto store = qq::TokenStore{client, test_credentials(), store_options(server)};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    const auto now = std::chrono::steady_clock::now();

    auto first = co_await store.access_token(now);
    REQUIRE(first.has_value());

    // 7200s TTL with the default 300s refresh-ahead: 200s before expiry the
    // cached token no longer satisfies the window, so the store refreshes.
    auto second = co_await store.access_token(now + 7200s - 200s);
    REQUIRE(second.has_value());
    REQUIRE(*second == "tok-2");
  });

  REQUIRE(server.served_count() == 2);
  blocking.join();
}

TEST_CASE("token store coerces string expires_in and floors tiny TTLs", "[unit][channel-qq][token]") {
  test::ScriptedHttpServer server{{token_ok("tok-1", R"("30")"), token_ok("tok-2")}};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};
  auto options = store_options(server);
  options.refresh_ahead = 10s;
  auto store = qq::TokenStore{client, test_credentials(), std::move(options)};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    const auto now = std::chrono::steady_clock::now();

    auto first = co_await store.access_token(now);
    REQUIRE(first.has_value());

    // The 30s string TTL is floored to 60s: at now+30s the token is still
    // outside the 10s refresh-ahead window, so no second request fires.
    auto cached = co_await store.access_token(now + 30s);
    REQUIRE(cached.has_value());
    REQUIRE(*cached == "tok-1");

    // At now+55s the floored expiry (now+60s) is inside the window.
    auto refreshed = co_await store.access_token(now + 55s);
    REQUIRE(refreshed.has_value());
    REQUIRE(*refreshed == "tok-2");
  });

  REQUIRE(server.served_count() == 2);
  blocking.join();
}

TEST_CASE("token store maps endpoint failures without leaking bodies", "[unit][channel-qq][token]") {
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};

  SECTION("2xx body without access_token is a credential rejection") {
    test::ScriptedHttpServer server{{test::ScriptedResponse{
        .status = 200,
        .body = R"({"code":100007,"message":"appid invalid"})",
        .headers = {},
        .delay = 0ms,
    }}};
    auto store = qq::TokenStore{client, test_credentials(), store_options(server)};
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto token = co_await store.access_token(std::chrono::steady_clock::now());
      REQUIRE_FALSE(token.has_value());
      REQUIRE(token.error().kind() == core::ErrorKind::auth);
      REQUIRE(context_value(token.error(), "biz_code") == "100007");
      REQUIRE(context_value(token.error(), "biz_message") == "appid invalid");
      REQUIRE(context_value(token.error(), "reason") == "missing_access_token");
    });
  }

  SECTION("4xx maps to auth") {
    test::ScriptedHttpServer server{{test::ScriptedResponse{.status = 400, .body = "{}", .headers = {}, .delay = 0ms}}};
    auto store = qq::TokenStore{client, test_credentials(), store_options(server)};
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto token = co_await store.access_token(std::chrono::steady_clock::now());
      REQUIRE_FALSE(token.has_value());
      REQUIRE(token.error().kind() == core::ErrorKind::auth);
      REQUIRE(context_value(token.error(), "http_status") == "400");
    });
  }

  SECTION("5xx maps to upstream") {
    test::ScriptedHttpServer server{{test::ScriptedResponse{.status = 503, .body = "", .headers = {}, .delay = 0ms}}};
    auto store = qq::TokenStore{client, test_credentials(), store_options(server)};
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto token = co_await store.access_token(std::chrono::steady_clock::now());
      REQUIRE_FALSE(token.has_value());
      REQUIRE(token.error().kind() == core::ErrorKind::upstream);
    });
  }

  blocking.join();
}

TEST_CASE("token store single-flights concurrent refreshes", "[unit][channel-qq][token]") {
  test::ScriptedHttpServer server{{token_ok("tok-1", "7200", /*delay=*/80ms)}};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};
  auto store = qq::TokenStore{client, test_credentials(), store_options(server)};

  test::run_async(
      [&](asio::io_context&) -> async::Awaitable<void> {
        using namespace asio::experimental::awaitable_operators;
        const auto now = std::chrono::steady_clock::now();
        core::Result<std::string> first = std::unexpected(core::Error::internal("unset"));
        core::Result<std::string> second = std::unexpected(core::Error::internal("unset"));

        auto fetch = [&](core::Result<std::string>& slot) -> async::Awaitable<void> {
          slot = co_await store.access_token(now);
        };
        co_await (fetch(first) && fetch(second));

        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        REQUIRE(*first == "tok-1");
        REQUIRE(*second == "tok-1");
      },
      /*timeout_duration=*/3s);

  REQUIRE(server.served_count() == 1);
  REQUIRE(store.refresh_count() == 1);
  blocking.join();
}

TEST_CASE("token store shares a failed refresh outcome with waiters", "[unit][channel-qq][token]") {
  test::ScriptedHttpServer server{{test::ScriptedResponse{.status = 500, .body = "", .headers = {}, .delay = 60ms}}};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};
  auto store = qq::TokenStore{client, test_credentials(), store_options(server)};

  test::run_async(
      [&](asio::io_context&) -> async::Awaitable<void> {
        using namespace asio::experimental::awaitable_operators;
        const auto now = std::chrono::steady_clock::now();
        core::Result<std::string> first{};
        core::Result<std::string> second{};

        auto fetch = [&](core::Result<std::string>& slot) -> async::Awaitable<void> {
          slot = co_await store.access_token(now);
        };
        co_await (fetch(first) && fetch(second));

        REQUIRE_FALSE(first.has_value());
        REQUIRE_FALSE(second.has_value());
        REQUIRE(first.error().kind() == core::ErrorKind::upstream);
        REQUIRE(second.error().kind() == core::ErrorKind::upstream);
      },
      /*timeout_duration=*/3s);

  REQUIRE(server.served_count() == 1);
  blocking.join();
}

TEST_CASE("token store invalidate forces a refetch", "[unit][channel-qq][token]") {
  test::ScriptedHttpServer server{{token_ok("tok-1"), token_ok("tok-2")}};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};
  auto store = qq::TokenStore{client, test_credentials(), store_options(server)};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    const auto now = std::chrono::steady_clock::now();

    auto first = co_await store.access_token(now);
    REQUIRE(first.has_value());

    store.invalidate();

    auto second = co_await store.access_token(now);
    REQUIRE(second.has_value());
    REQUIRE(*second == "tok-2");
  });

  REQUIRE(server.served_count() == 2);
  blocking.join();
}

TEST_CASE("token store observes cancellation mid-refresh", "[unit][channel-qq][token]") {
  test::ScriptedHttpServer server{{token_ok("tok-1", "7200", /*delay=*/400ms)}};
  asio::io_context io;
  asio::thread_pool blocking{1};
  asio::cancellation_signal signal;
  auto client = http::Client{blocking.get_executor()};
  auto store = qq::TokenStore{client, test_credentials(), store_options(server)};
  std::optional<core::Result<std::string>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<std::string>> {
        co_await asio::this_coro::throw_if_cancelled(false);
        co_return co_await store.access_token(std::chrono::steady_clock::now());
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<std::string> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::steady_timer cancel_timer{io};
  cancel_timer.expires_after(50ms);
  cancel_timer.async_wait([&](const asio::error_code& ec) {
    if (!ec) {
      signal.emit(asio::cancellation_type::terminal);
    }
  });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  blocking.join();
}

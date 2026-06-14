// tests/channel-qq/test_api_client.cpp — QQ API client request/retry/normalize coverage.

#include <oran/channel-qq/api_client.hpp>

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
#include <asio/io_context.hpp>
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

constexpr std::string_view kTokenPath = "/app/getAppAccessToken";

[[nodiscard]] std::optional<std::string_view> context_value(const core::Error& error, std::string_view key) {
  for (const auto& [name, value] : error.context()) {
    if (name == key) {
      return value;
    }
  }
  return std::nullopt;
}

[[nodiscard]] test::ScriptedResponse token_ok(std::string token) {
  return test::ScriptedResponse{
      .status = 200,
      .body = R"({"access_token":")" + std::move(token) + R"(","expires_in":7200})",
      .headers = {},
      .delay = 0ms,
  };
}

[[nodiscard]] test::ScriptedResponse
api_response(int status, std::string body, std::vector<std::pair<std::string, std::string>> headers = {}) {
  return test::ScriptedResponse{.status = status, .body = std::move(body), .headers = std::move(headers), .delay = 0ms};
}

struct ClientFixture {
  explicit ClientFixture(const test::ScriptedHttpServer& server, qq::ApiClientOptions overrides = {})
      : client{blocking.get_executor()},
        tokens{client, qq::Credentials{.app_id = "test-app", .client_secret = "test-secret"}, token_options(server)},
        api{client, tokens, api_options(server, std::move(overrides))} {}

  [[nodiscard]] static qq::TokenStoreOptions token_options(const test::ScriptedHttpServer& server) {
    auto options = qq::TokenStoreOptions{};
    options.token_url = server.url(kTokenPath);
    options.request_timeout = 2s;
    return options;
  }

  [[nodiscard]] static qq::ApiClientOptions api_options(const test::ScriptedHttpServer& server,
                                                        qq::ApiClientOptions overrides) {
    overrides.base_url = server.base_url();
    overrides.request_timeout = 2s;
    return overrides;
  }

  asio::thread_pool blocking{1};
  http::Client client;
  qq::TokenStore tokens;
  qq::ApiClient api;
};

}  // namespace

TEST_CASE("api client sends authenticated GET requests", "[unit][channel-qq][api]") {
  test::ScriptedHttpServer server{{
      token_ok("tok-1"),
      api_response(200, R"({"url":"wss://gateway.example"})", {{"x-tps-trace-id", "trace-123"}}),
  }};
  ClientFixture fixture{server};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await fixture.api.get("/gateway");
    REQUIRE(response.has_value());
    REQUIRE(response->http_status == 200);
    REQUIRE(response->body == R"({"url":"wss://gateway.example"})");
    REQUIRE(response->trace_id == "trace-123");
    REQUIRE(response->biz_code == 0);
  });

  REQUIRE(server.served_count() == 2);
  const auto request = server.request_text(1);
  REQUIRE(request.starts_with("GET /gateway HTTP/1.1"));
  REQUIRE(request.contains("Authorization: QQBot tok-1"));
  REQUIRE(request.contains("User-Agent: orangutan/qq-channel"));
  fixture.blocking.join();
}

TEST_CASE("api client discovers the gateway bot endpoint", "[unit][channel-qq][api]") {
  test::ScriptedHttpServer server{{
      token_ok("tok-1"),
      api_response(
          200,
          R"({"url":"wss://gateway.qq.example","shards":2,"session_start_limit":{"total":1000,"remaining":999,"reset_after":14400000,"max_concurrency":16}})"),
  }};
  ClientFixture fixture{server};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto endpoint = co_await qq::discover_gateway_bot(fixture.api);
    REQUIRE(endpoint.has_value());
    REQUIRE(endpoint->url == "wss://gateway.qq.example");
    REQUIRE(endpoint->shards == 2);
    REQUIRE(endpoint->session_start_limit.has_value());
    REQUIRE(endpoint->session_start_limit->total == 1000);
    REQUIRE(endpoint->session_start_limit->remaining == 999);
    REQUIRE(endpoint->session_start_limit->reset_after == 14'400'000ms);
    REQUIRE(endpoint->session_start_limit->max_concurrency == 16);
  });

  REQUIRE(server.served_count() == 2);
  const auto request = server.request_text(1);
  REQUIRE(request.starts_with("GET /gateway/bot HTTP/1.1"));
  REQUIRE(request.contains("Authorization: QQBot tok-1"));
  fixture.blocking.join();
}

TEST_CASE("gateway bot response parser defaults optional fields", "[unit][channel-qq][api]") {
  auto endpoint = qq::parse_gateway_bot_response(R"({"url":"ws://127.0.0.1/gateway"})");
  REQUIRE(endpoint.has_value());
  REQUIRE(endpoint->url == "ws://127.0.0.1/gateway");
  REQUIRE(endpoint->shards == 1);
  REQUIRE_FALSE(endpoint->session_start_limit.has_value());
}

TEST_CASE("gateway bot response parser rejects unusable discovery payloads", "[unit][channel-qq][api]") {
  SECTION("missing url") {
    auto endpoint = qq::parse_gateway_bot_response(R"({"shards":1})");
    REQUIRE_FALSE(endpoint.has_value());
    REQUIRE(endpoint.error().kind() == core::ErrorKind::parsing);
    REQUIRE(context_value(endpoint.error(), "field") == "url");
  }

  SECTION("non-websocket url") {
    auto endpoint = qq::parse_gateway_bot_response(R"({"url":"https://gateway.qq.example"})");
    REQUIRE_FALSE(endpoint.has_value());
    REQUIRE(endpoint.error().kind() == core::ErrorKind::parsing);
    REQUIRE(context_value(endpoint.error(), "field") == "url");
  }

  SECTION("invalid session limit") {
    auto endpoint =
        qq::parse_gateway_bot_response(R"({"url":"wss://gateway.qq.example","session_start_limit":{"remaining":-1}})");
    REQUIRE_FALSE(endpoint.has_value());
    REQUIRE(endpoint.error().kind() == core::ErrorKind::parsing);
    REQUIRE(context_value(endpoint.error(), "field") == "remaining");
  }
}

TEST_CASE("api client posts serialized JSON bodies", "[unit][channel-qq][api]") {
  test::ScriptedHttpServer server{{token_ok("tok-1"), api_response(200, R"({"id":"msg-1"})")}};
  ClientFixture fixture{server};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await fixture.api.post("/channels/42/messages", R"({"content":"hello"})");
    REQUIRE(response.has_value());
    REQUIRE(response->http_status == 200);
  });

  const auto request = server.request_text(1);
  REQUIRE(request.starts_with("POST /channels/42/messages HTTP/1.1"));
  REQUIRE(request.contains("Content-Type: application/json"));
  REQUIRE(request.contains(R"({"content":"hello"})"));
  fixture.blocking.join();
}

TEST_CASE("api client refreshes the token once on 401", "[unit][channel-qq][api]") {
  test::ScriptedHttpServer server{{
      token_ok("tok-1"),
      api_response(401, ""),
      token_ok("tok-2"),
      api_response(200, "{}"),
  }};
  ClientFixture fixture{server};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await fixture.api.get("/gateway");
    REQUIRE(response.has_value());
    REQUIRE(response->http_status == 200);
  });

  REQUIRE(server.served_count() == 4);
  REQUIRE(server.request_text(1).contains("Authorization: QQBot tok-1"));
  REQUIRE(server.request_text(2).starts_with("POST /app/getAppAccessToken"));
  REQUIRE(server.request_text(3).contains("Authorization: QQBot tok-2"));
  fixture.blocking.join();
}

TEST_CASE("api client maps a repeated 401 to auth", "[unit][channel-qq][api]") {
  test::ScriptedHttpServer server{{
      token_ok("tok-1"),
      api_response(401, ""),
      token_ok("tok-2"),
      api_response(401, ""),
  }};
  ClientFixture fixture{server};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await fixture.api.get("/gateway");
    REQUIRE_FALSE(response.has_value());
    REQUIRE(response.error().kind() == core::ErrorKind::auth);
    REQUIRE(context_value(response.error(), "http_status") == "401");
    REQUIRE(context_value(response.error(), "path") == "/gateway");
  });

  REQUIRE(server.served_count() == 4);
  fixture.blocking.join();
}

TEST_CASE("api client honors retry-after on 429", "[unit][channel-qq][api]") {
  test::ScriptedHttpServer server{{
      token_ok("tok-1"),
      api_response(429, "", {{"Retry-After", "0.05"}}),
      api_response(200, "{}"),
  }};
  ClientFixture fixture{server};

  test::run_async(
      [&](asio::io_context&) -> async::Awaitable<void> {
        auto response = co_await fixture.api.get("/gateway");
        REQUIRE(response.has_value());
        REQUIRE(response->http_status == 200);
      },
      /*timeout_duration=*/3s);

  REQUIRE(server.served_count() == 3);
  fixture.blocking.join();
}

TEST_CASE("api client surfaces rate limits when retries are exhausted", "[unit][channel-qq][api]") {
  test::ScriptedHttpServer server{{
      token_ok("tok-1"),
      api_response(429, "", {{"Retry-After", "2.5"}}),
  }};
  auto overrides = qq::ApiClientOptions{};
  overrides.max_rate_limit_retries = 0;
  ClientFixture fixture{server, std::move(overrides)};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await fixture.api.get("/gateway");
    REQUIRE_FALSE(response.has_value());
    REQUIRE(response.error().kind() == core::ErrorKind::rate_limit);
    const auto retry_after = response.error().retry_after();
    REQUIRE(retry_after.has_value());
    REQUIRE(*retry_after == 2500ms);
  });

  fixture.blocking.join();
}

TEST_CASE("api client retries transient gateway failures", "[unit][channel-qq][api]") {
  test::ScriptedHttpServer server{{
      token_ok("tok-1"),
      api_response(502, ""),
      api_response(200, "{}"),
  }};
  auto overrides = qq::ApiClientOptions{};
  overrides.gateway_retry_base_delay = 10ms;
  ClientFixture fixture{server, std::move(overrides)};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await fixture.api.get("/gateway");
    REQUIRE(response.has_value());
    REQUIRE(response->http_status == 200);
  });

  REQUIRE(server.served_count() == 3);
  fixture.blocking.join();
}

TEST_CASE("api client maps business-error envelopes to upstream", "[unit][channel-qq][api]") {
  test::ScriptedHttpServer server{{
      token_ok("tok-1"),
      api_response(200, R"({"code":11244,"message":"bot not in guild"})"),
  }};
  ClientFixture fixture{server};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await fixture.api.post("/guilds/1/messages", "{}");
    REQUIRE_FALSE(response.has_value());
    REQUIRE(response.error().kind() == core::ErrorKind::upstream);
    REQUIRE(context_value(response.error(), "biz_code") == "11244");
    REQUIRE(context_value(response.error(), "biz_message") == "bot not in guild");
  });

  fixture.blocking.join();
}

TEST_CASE("api client maps status classes onto error kinds", "[unit][channel-qq][api]") {
  SECTION("404 is not_found") {
    test::ScriptedHttpServer server{{token_ok("tok-1"), api_response(404, "")}};
    ClientFixture fixture{server};
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await fixture.api.get("/channels/missing");
      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::not_found);
    });
    fixture.blocking.join();
  }

  SECTION("other 4xx is invalid_argument") {
    test::ScriptedHttpServer server{{token_ok("tok-1"), api_response(400, "")}};
    ClientFixture fixture{server};
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await fixture.api.get("/gateway");
      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::invalid_argument);
    });
    fixture.blocking.join();
  }

  SECTION("non-gateway 500 is upstream without retry") {
    test::ScriptedHttpServer server{{token_ok("tok-1"), api_response(500, "")}};
    ClientFixture fixture{server};
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await fixture.api.get("/gateway");
      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::upstream);
    });
    fixture.blocking.join();
  }
}

TEST_CASE("api client requests absolute URLs without base joining", "[unit][channel-qq][api]") {
  test::ScriptedHttpServer server{{token_ok("tok-1"), api_response(200, "{}")}};
  auto overrides = qq::ApiClientOptions{};
  overrides.base_url = "http://192.0.2.1:1";  // unroutable — must not be contacted
  ClientFixture fixture{server, std::move(overrides)};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto response = co_await fixture.api.get(server.url("/absolute/path"));
    REQUIRE(response.has_value());
    REQUIRE(response->http_status == 200);
  });

  REQUIRE(server.request_text(1).starts_with("GET /absolute/path HTTP/1.1"));
  fixture.blocking.join();
}

TEST_CASE("api client observes cancellation during rate-limit backoff", "[unit][channel-qq][api]") {
  test::ScriptedHttpServer server{{
      token_ok("tok-1"),
      api_response(429, "", {{"Retry-After", "10"}}),
  }};
  ClientFixture fixture{server};

  asio::io_context io;
  asio::cancellation_signal signal;
  std::optional<core::Result<qq::ApiResponse>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<qq::ApiResponse>> {
        co_await asio::this_coro::throw_if_cancelled(false);
        co_return co_await fixture.api.get("/gateway");
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<qq::ApiResponse> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::steady_timer cancel_timer{io};
  cancel_timer.expires_after(150ms);
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
  fixture.blocking.join();
}

TEST_CASE("normalize_api_response captures headers case-insensitively", "[unit][channel-qq][api]") {
  auto response = http::BodyResponse{
      .status_code = 429,
      .headers = {{.name = "X-TPS-Trace-ID", .value = "trace-xyz"}, {.name = "RETRY-AFTER", .value = "1.5"}},
      .body = R"({"code":11264,"message":"rate limited"})",
  };

  const auto normalized = qq::normalize_api_response(std::move(response));
  REQUIRE(normalized.http_status == 429);
  REQUIRE(normalized.trace_id == "trace-xyz");
  REQUIRE(normalized.retry_after == "1.5");
  REQUIRE(normalized.biz_code == 11264);
  REQUIRE(normalized.biz_message == "rate limited");
}

TEST_CASE("normalize_api_response tolerates empty and non-JSON bodies", "[unit][channel-qq][api]") {
  SECTION("empty body") {
    const auto normalized =
        qq::normalize_api_response(http::BodyResponse{.status_code = 204, .headers = {}, .body = ""});
    REQUIRE(normalized.http_status == 204);
    REQUIRE(normalized.biz_code == 0);
  }

  SECTION("non-JSON body") {
    const auto normalized =
        qq::normalize_api_response(http::BodyResponse{.status_code = 200, .headers = {}, .body = "<html>oops</html>"});
    REQUIRE(normalized.body == "<html>oops</html>");
    REQUIRE(normalized.biz_code == 0);
  }
}

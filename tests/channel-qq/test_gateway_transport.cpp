// tests/channel-qq/test_gateway_transport.cpp — QQ gateway WebSocket driver.

#include <oran/channel-qq/gateway_transport.hpp>
#include <oran/channel-qq/token_store.hpp>

#include <chrono>
#include <exception>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/http/client.hpp>

#include "../test-helpers/run_async.hpp"
#include "../test-helpers/ws_test_server.hpp"
#include "scripted_http_server.hpp"

using namespace std::chrono_literals;

namespace {

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace http = orangutan::http;
namespace qq = orangutan::channel::qq;
namespace test = orangutan::tests;
namespace ws = orangutan::tests::ws;

using json = nlohmann::json;

[[nodiscard]] test::ScriptedResponse token_ok(std::string token) {
  return test::ScriptedResponse{
      .status = 200,
      .body = R"({"access_token":")" + std::move(token) + R"(","expires_in":7200})",
      .headers = {{"Content-Type", "application/json"}},
      .delay = 0ms,
  };
}

[[nodiscard]] qq::TokenStoreOptions token_options(const test::ScriptedHttpServer& server) {
  auto options = qq::TokenStoreOptions{};
  options.token_url = server.url("/app/getAppAccessToken");
  options.request_timeout = 2s;
  return options;
}

[[nodiscard]] qq::GatewayTransportOptions transport_options(const ws::ScriptedWsServer& server) {
  auto options = qq::GatewayTransportOptions{};
  options.gateway_url = server.url();
  options.handshake_timeout = 2s;
  options.frame_timeout = 300ms;
  options.reconnect_delays = {1ms, 1ms, 1ms, 1ms, 1ms, 1ms};
  options.max_reconnect_attempts = 5;
  return options;
}

[[nodiscard]] std::string hello_frame(std::chrono::milliseconds interval) {
  return json{{"op", 10}, {"d", {{"heartbeat_interval", interval.count()}}}}.dump();
}

[[nodiscard]] std::string ready_frame(std::uint32_t seq, std::string session_id) {
  return json{{"op", 0}, {"s", seq}, {"t", "READY"}, {"d", {{"session_id", std::move(session_id)}}}}.dump();
}

[[nodiscard]] std::string resumed_frame() {
  return json{{"op", 0}, {"t", "RESUMED"}, {"d", json::object()}}.dump();
}

[[nodiscard]] std::string dispatch_frame(std::uint32_t seq, std::string event_type, json data) {
  return json{{"op", 0}, {"s", seq}, {"t", std::move(event_type)}, {"d", std::move(data)}}.dump();
}

struct TransportFixture {
  explicit TransportFixture(const test::ScriptedHttpServer& token_server, const ws::ScriptedWsServer& gateway_server)
      : client{blocking.get_executor()}, tokens{client,
                                                qq::Credentials{.app_id = "test-app", .client_secret = "test-secret"},
                                                token_options(token_server)},
        transport{tokens, transport_options(gateway_server)} {}

  asio::thread_pool blocking{1};
  http::Client client;
  qq::TokenStore tokens;
  qq::GatewayTransport transport;
};

}  // namespace

TEST_CASE("gateway transport identifies, heartbeats, and returns dispatches", "[unit][channel-qq][gateway]") {
  test::ScriptedHttpServer token_server{{token_ok("tok-1")}};
  ws::ScriptedWsServer gateway_server{{{
      ws::SendText{hello_frame(40ms)},
      ws::RecvFrame{},
      ws::SendText{ready_frame(1, "sess-1")},
      ws::Delay{80ms},
      ws::RecvFrame{},
      ws::SendText{dispatch_frame(2, "GROUP_AT_MESSAGE_CREATE", json{{"content", "hello"}, {"group_openid", "g-1"}})},
  }}};
  TransportFixture fixture{token_server, gateway_server};

  test::run_async(
      [&](asio::io_context&) -> async::Awaitable<void> {
        auto dispatch = co_await fixture.transport.next_dispatch();
        REQUIRE(dispatch.has_value());
        REQUIRE(dispatch->event_type == "GROUP_AT_MESSAGE_CREATE");
        const auto data = json::parse(dispatch->data_json);
        REQUIRE(data.at("content") == "hello");
        REQUIRE(fixture.transport.session_id() == "sess-1");
        REQUIRE(fixture.transport.last_seq() == 2);
      },
      3s);

  const auto frames = gateway_server.recorded_frames();
  REQUIRE(frames.size() == 2);
  REQUIRE(frames[0].opcode == 0x1);
  const auto identify = json::parse(frames[0].payload);
  REQUIRE(identify.at("op") == 2);
  REQUIRE(identify.at("d").at("token") == "QQBot tok-1");

  REQUIRE(frames[1].opcode == 0x1);
  const auto heartbeat = json::parse(frames[1].payload);
  REQUIRE(heartbeat.at("op") == 1);
  REQUIRE(heartbeat.at("d") == 1);
  fixture.blocking.join();
}

TEST_CASE("gateway transport resumes after a resumable close", "[unit][channel-qq][gateway]") {
  test::ScriptedHttpServer token_server{{token_ok("tok-1")}};
  ws::ScriptedWsServer gateway_server{{
      {
          ws::SendText{hello_frame(200ms)},
          ws::RecvFrame{},
          ws::SendText{ready_frame(5, "sess-1")},
          ws::SendClose{.code = 4009, .reason = "session timeout"},
      },
      {
          ws::SendText{hello_frame(200ms)},
          ws::RecvFrame{},
          ws::SendText{resumed_frame()},
          ws::SendText{dispatch_frame(6, "C2C_MESSAGE_CREATE", json{{"content", "after resume"}})},
      },
  }};
  TransportFixture fixture{token_server, gateway_server};

  test::run_async(
      [&](asio::io_context&) -> async::Awaitable<void> {
        auto dispatch = co_await fixture.transport.next_dispatch();
        REQUIRE(dispatch.has_value());
        REQUIRE(dispatch->event_type == "C2C_MESSAGE_CREATE");
        REQUIRE(fixture.transport.session_id() == "sess-1");
        REQUIRE(fixture.transport.last_seq() == 6);
      },
      3s);

  REQUIRE(gateway_server.accepted_count() == 2);
  const auto frames = gateway_server.recorded_frames();
  REQUIRE(frames.size() == 2);
  REQUIRE(json::parse(frames[0].payload).at("op") == 2);
  const auto resume = json::parse(frames[1].payload);
  REQUIRE(resume.at("op") == 6);
  REQUIRE(resume.at("d").at("session_id") == "sess-1");
  REQUIRE(resume.at("d").at("seq") == 5);
  fixture.blocking.join();
}

TEST_CASE("gateway transport refreshes the token after auth close", "[unit][channel-qq][gateway]") {
  test::ScriptedHttpServer token_server{{token_ok("tok-1"), token_ok("tok-2")}};
  ws::ScriptedWsServer gateway_server{{
      {
          ws::SendText{hello_frame(200ms)},
          ws::RecvFrame{},
          ws::SendClose{.code = 4004, .reason = "auth failed"},
      },
      {
          ws::SendText{hello_frame(200ms)},
          ws::RecvFrame{},
          ws::SendText{ready_frame(1, "sess-2")},
          ws::SendText{dispatch_frame(2, "GROUP_AT_MESSAGE_CREATE", json{{"content", "fresh"}})},
      },
  }};
  TransportFixture fixture{token_server, gateway_server};

  test::run_async(
      [&](asio::io_context&) -> async::Awaitable<void> {
        auto dispatch = co_await fixture.transport.next_dispatch();
        REQUIRE(dispatch.has_value());
        REQUIRE(dispatch->event_type == "GROUP_AT_MESSAGE_CREATE");
        REQUIRE(fixture.tokens.refresh_count() == 2);
      },
      3s);

  const auto frames = gateway_server.recorded_frames();
  REQUIRE(frames.size() == 2);
  REQUIRE(json::parse(frames[0].payload).at("d").at("token") == "QQBot tok-1");
  REQUIRE(json::parse(frames[1].payload).at("d").at("token") == "QQBot tok-2");
  fixture.blocking.join();
}

TEST_CASE("gateway transport surfaces token failures before reconnecting", "[unit][channel-qq][gateway]") {
  test::ScriptedHttpServer token_server{
      {test::ScriptedResponse{.status = 400, .body = "{}", .headers = {}, .delay = 0ms}}};
  ws::ScriptedWsServer gateway_server{{{
      ws::SendText{hello_frame(200ms)},
  }}};
  TransportFixture fixture{token_server, gateway_server};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto dispatch = co_await fixture.transport.next_dispatch();
    REQUIRE_FALSE(dispatch.has_value());
    REQUIRE(dispatch.error().kind() == core::ErrorKind::auth);
  });

  REQUIRE(token_server.served_count() == 1);
  REQUIRE(gateway_server.accepted_count() == 1);
  REQUIRE(gateway_server.recorded_frames().empty());
  fixture.blocking.join();
}

TEST_CASE("gateway transport observes cancellation while waiting for dispatch", "[unit][channel-qq][gateway]") {
  test::ScriptedHttpServer token_server{{token_ok("tok-1")}};
  ws::ScriptedWsServer gateway_server{{{
      ws::SendText{hello_frame(5s)},
      ws::RecvFrame{},
      ws::Delay{2s},
  }}};
  TransportFixture fixture{token_server, gateway_server};

  asio::io_context io;
  asio::cancellation_signal signal;
  std::optional<core::Result<qq::GatewayDispatch>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<qq::GatewayDispatch>> {
        co_await asio::this_coro::throw_if_cancelled(false);
        co_return co_await fixture.transport.next_dispatch();
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<qq::GatewayDispatch> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::steady_timer cancel_after{io};
  cancel_after.expires_after(100ms);
  cancel_after.async_wait([&](const asio::error_code& ec) {
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

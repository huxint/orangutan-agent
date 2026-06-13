// tests/http/test_websocket.cpp — WebSocket boundary coverage against the
// scripted loopback server (handshake, framing, reassembly, close, timeout,
// cancellation).

#include <oran/http/websocket.hpp>

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
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/error.hpp>

#include "../test-helpers/run_async.hpp"
#include "../test-helpers/ws_test_server.hpp"

using namespace std::chrono_literals;

namespace {

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace http = orangutan::http;
namespace test = orangutan::tests;
namespace ws = orangutan::tests::ws;

[[nodiscard]] http::WsConnectRequest request_for(const ws::ScriptedWsServer& server) {
  return http::WsConnectRequest{.url = server.url(), .headers = {}, .handshake_timeout = 2000ms};
}

}  // namespace

TEST_CASE("WebSocket validates the connect request shape", "[unit][http][websocket]") {
  SECTION("empty url") {
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto socket = co_await http::WebSocket::connect(
          http::WsConnectRequest{.url = "", .headers = {}, .handshake_timeout = 2000ms});
      REQUIRE_FALSE(socket.has_value());
      REQUIRE(socket.error().kind() == core::ErrorKind::invalid_argument);
    });
  }

  SECTION("non-websocket scheme") {
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto socket = co_await http::WebSocket::connect(
          http::WsConnectRequest{.url = "https://example.com/gateway", .headers = {}, .handshake_timeout = 2000ms});
      REQUIRE_FALSE(socket.has_value());
      REQUIRE(socket.error().kind() == core::ErrorKind::invalid_argument);
    });
  }

  SECTION("non-positive handshake timeout") {
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto socket = co_await http::WebSocket::connect(
          http::WsConnectRequest{.url = "ws://127.0.0.1:1/", .headers = {}, .handshake_timeout = 0ms});
      REQUIRE_FALSE(socket.has_value());
      REQUIRE(socket.error().kind() == core::ErrorKind::invalid_argument);
    });
  }
}

TEST_CASE("WebSocket completes the upgrade handshake and exchanges text", "[unit][http][websocket]") {
  ws::ScriptedWsServer server{{{
      ws::RecvFrame{},
      ws::SendText{R"({"op":10,"d":{"heartbeat_interval":41250}})"},
  }}};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto request = request_for(server);
    request.headers.push_back(http::Header{.name = "Authorization", .value = "QQBot test-token"});

    auto socket = co_await http::WebSocket::connect(std::move(request));
    if (!socket) {
      UNSCOPED_INFO("connect error: " << std::string{socket.error().message()});
      for (const auto& [key, value] : socket.error().context()) {
        UNSCOPED_INFO("  " << key << "=" << value);
      }
    }
    REQUIRE(socket.has_value());

    auto sent = co_await socket->send_text(R"({"op":1,"d":null})");
    REQUIRE(sent.has_value());

    auto received = co_await socket->receive(2000ms);
    REQUIRE(received.has_value());
    REQUIRE(received->has_value());
    REQUIRE((*received)->kind == http::WsMessageKind::text);
    REQUIRE((*received)->payload == R"({"op":10,"d":{"heartbeat_interval":41250}})");
  });

  REQUIRE(server.accepted_count() == 1);
  const auto handshake = server.handshake_request(0);
  REQUIRE(handshake.contains("Upgrade: websocket"));
  REQUIRE(handshake.contains("Sec-WebSocket-Key:"));
  REQUIRE(handshake.contains("Authorization: QQBot test-token"));

  const auto frames = server.recorded_frames();
  REQUIRE(frames.size() == 1);
  REQUIRE(frames[0].opcode == 0x1);
  REQUIRE(frames[0].payload == R"({"op":1,"d":null})");
}

TEST_CASE("WebSocket receive returns nullopt on timeout and the message later", "[unit][http][websocket]") {
  ws::ScriptedWsServer server{{{
      ws::Delay{300ms},
      ws::SendText{"late"},
  }}};

  test::run_async(
      [&](asio::io_context&) -> async::Awaitable<void> {
        auto socket = co_await http::WebSocket::connect(request_for(server));
        REQUIRE(socket.has_value());

        auto first = co_await socket->receive(50ms);
        REQUIRE(first.has_value());
        REQUIRE_FALSE(first->has_value());

        auto second = co_await socket->receive(2000ms);
        REQUIRE(second.has_value());
        REQUIRE(second->has_value());
        REQUIRE((*second)->payload == "late");
      },
      5s);
}

TEST_CASE("WebSocket surfaces a peer close frame once", "[unit][http][websocket]") {
  ws::ScriptedWsServer server{{{
      ws::SendClose{.code = 4009, .reason = "session timed out, resume"},
  }}};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto socket = co_await http::WebSocket::connect(request_for(server));
    REQUIRE(socket.has_value());

    auto received = co_await socket->receive(2000ms);
    REQUIRE(received.has_value());
    REQUIRE(received->has_value());
    REQUIRE((*received)->kind == http::WsMessageKind::close);
    REQUIRE((*received)->close_code == 4009);
    REQUIRE((*received)->payload == "session timed out, resume");

    auto after_close = co_await socket->receive(100ms);
    REQUIRE_FALSE(after_close.has_value());
    REQUIRE(after_close.error().kind() == core::ErrorKind::conflict);

    auto send_after_close = co_await socket->send_text("nope");
    REQUIRE_FALSE(send_after_close.has_value());
    REQUIRE(send_after_close.error().kind() == core::ErrorKind::conflict);
  });
}

TEST_CASE("WebSocket reassembles fragmented messages", "[unit][http][websocket]") {
  ws::ScriptedWsServer server{{{
      ws::SendFragmentedText{{"alpha-", "beta-", "gamma"}},
  }}};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto socket = co_await http::WebSocket::connect(request_for(server));
    REQUIRE(socket.has_value());

    auto received = co_await socket->receive(2000ms);
    REQUIRE(received.has_value());
    REQUIRE(received->has_value());
    REQUIRE((*received)->kind == http::WsMessageKind::text);
    REQUIRE((*received)->payload == "alpha-beta-gamma");
  });
}

TEST_CASE("WebSocket reassembles a message larger than one receive chunk", "[unit][http][websocket]") {
  const auto large = std::string(70'000, 'x');
  ws::ScriptedWsServer server{{{
      ws::SendText{large},
  }}};

  test::run_async(
      [&](asio::io_context&) -> async::Awaitable<void> {
        auto socket = co_await http::WebSocket::connect(request_for(server));
        REQUIRE(socket.has_value());

        auto received = co_await socket->receive(2000ms);
        REQUIRE(received.has_value());
        REQUIRE(received->has_value());
        REQUIRE((*received)->payload.size() == large.size());
        REQUIRE((*received)->payload == large);
      },
      5s);
}

TEST_CASE("WebSocket keeps ping/pong transparent to the caller", "[unit][http][websocket]") {
  ws::ScriptedWsServer server{{{
      ws::SendPing{"keepalive"},
      ws::RecvFrame{},  // libcurl's automatic pong
      ws::SendText{"after-ping"},
  }}};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto socket = co_await http::WebSocket::connect(request_for(server));
    REQUIRE(socket.has_value());

    auto received = co_await socket->receive(2000ms);
    REQUIRE(received.has_value());
    REQUIRE(received->has_value());
    REQUIRE((*received)->kind == http::WsMessageKind::text);
    REQUIRE((*received)->payload == "after-ping");
  });

  const auto frames = server.recorded_frames();
  REQUIRE(frames.size() == 1);
  REQUIRE(frames[0].opcode == 0xA);
  REQUIRE(frames[0].payload == "keepalive");
}

TEST_CASE("WebSocket surfaces binary messages with the binary kind", "[unit][http][websocket]") {
  ws::ScriptedWsServer server{{{
      ws::SendBinary{std::string{"\x00\x01\x02", 3}},
  }}};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto socket = co_await http::WebSocket::connect(request_for(server));
    REQUIRE(socket.has_value());

    auto received = co_await socket->receive(2000ms);
    REQUIRE(received.has_value());
    REQUIRE(received->has_value());
    REQUIRE((*received)->kind == http::WsMessageKind::binary);
    REQUIRE((*received)->payload == std::string{"\x00\x01\x02", 3});
  });
}

TEST_CASE("WebSocket close sends the status code and reason", "[unit][http][websocket]") {
  ws::ScriptedWsServer server{{{
      ws::RecvFrame{},
  }}};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto socket = co_await http::WebSocket::connect(request_for(server));
    REQUIRE(socket.has_value());

    auto closed = co_await socket->close(1000, "done");
    REQUIRE(closed.has_value());

    auto closed_again = co_await socket->close(1000, "twice");
    REQUIRE_FALSE(closed_again.has_value());
    REQUIRE(closed_again.error().kind() == core::ErrorKind::conflict);
  });

  const auto frames = server.recorded_frames();
  REQUIRE(frames.size() == 1);
  REQUIRE(frames[0].opcode == 0x8);
  REQUIRE(frames[0].payload.size() == 6);
  REQUIRE(static_cast<unsigned char>(frames[0].payload[0]) == 0x03);
  REQUIRE(static_cast<unsigned char>(frames[0].payload[1]) == 0xe8);
  REQUIRE(frames[0].payload.substr(2) == "done");
}

TEST_CASE("WebSocket connect fails when the server rejects the upgrade", "[unit][http][websocket]") {
  ws::ScriptedWsServer server{{{
      ws::RejectHandshake{.status = 404},
  }}};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto socket = co_await http::WebSocket::connect(request_for(server));
    REQUIRE_FALSE(socket.has_value());
    REQUIRE((socket.error().kind() == core::ErrorKind::network || socket.error().kind() == core::ErrorKind::upstream));
  });
}

TEST_CASE("WebSocket connect fails fast when nothing listens", "[unit][http][websocket]") {
  test::run_async(
      [&](asio::io_context&) -> async::Awaitable<void> {
        // Port 1 on loopback refuses immediately on Linux.
        auto socket = co_await http::WebSocket::connect(
            http::WsConnectRequest{.url = "ws://127.0.0.1:1/", .headers = {}, .handshake_timeout = 2000ms});
        REQUIRE_FALSE(socket.has_value());
        REQUIRE(socket.error().kind() == core::ErrorKind::network);
      },
      5s);
}

TEST_CASE("WebSocket receive observes cancellation while suspended", "[unit][http][websocket]") {
  ws::ScriptedWsServer server{{{
      ws::Delay{2000ms},
  }}};

  asio::io_context io;
  asio::cancellation_signal signal;
  std::optional<core::Result<std::optional<http::WsMessage>>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<std::optional<http::WsMessage>>> {
        co_await asio::this_coro::throw_if_cancelled(false);
        auto socket = co_await http::WebSocket::connect(request_for(server));
        if (!socket) {
          co_return std::unexpected(std::move(socket).error());
        }
        co_return co_await socket->receive(10'000ms);
      },
      asio::bind_cancellation_slot(signal.slot(),
                                   [&](std::exception_ptr ep, core::Result<std::optional<http::WsMessage>> r) {
                                     failure = ep;
                                     result = std::move(r);
                                     io.stop();
                                   }));

  asio::steady_timer cancel_after{io};
  cancel_after.expires_after(150ms);
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
}

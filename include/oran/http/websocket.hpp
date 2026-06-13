// include/oran/http/websocket.hpp — cancel-aware WebSocket boundary.
//
// A `WebSocket` is one connected `ws://` / `wss://` client connection built on
// libcurl's WebSocket API; the curl handle and socket details live in
// src/oran-http/websocket.cpp (C6). Unlike `Client::send`, nothing here posts
// to a blocking executor: the handshake is driven by short non-blocking
// `curl_multi_perform` rounds between cancel-aware timer ticks, and
// receive/send use libcurl's non-blocking connect-only mode, suspending on
// asio socket readiness instead of occupying a CPU-pool thread. A persistent
// gateway connection therefore costs no thread while idle — with the default
// `RuntimeConfig{.cpu_workers = 1}` a blocking receive loop would starve every
// other blocking task in the process.
//
// Not thread-safe by design: one coroutine owns a connection and runs at most
// one operation at a time (the same one-resume-per-call discipline the
// `channel::Channel` trait expects of its transport owner).

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/http/client.hpp>

namespace orangutan::http {

struct WsConnectRequest {
  std::string url;  // must be ws:// or wss://
  std::vector<Header> headers;
  std::chrono::milliseconds handshake_timeout{30000};

  friend bool operator==(const WsConnectRequest&, const WsConnectRequest&) = default;
};

enum class WsMessageKind : std::uint8_t {
  text,
  binary,
  close,
};

/// One complete WebSocket message, reassembled across fragments. For a
/// `close` message `close_code` carries the peer's status code (0 when the
/// close frame had no payload) and `payload` carries the close reason.
struct WsMessage {
  WsMessageKind kind{WsMessageKind::text};
  std::string payload;
  std::uint16_t close_code{0};

  friend bool operator==(const WsMessage&, const WsMessage&) = default;
};

class WebSocket {
public:
  /// Dial and complete the WebSocket handshake. Runs on the calling
  /// coroutine's executor; cancellation is observed between handshake
  /// progress rounds and `handshake_timeout` bounds the whole dial
  /// (DNS + TCP + TLS + HTTP upgrade).
  [[nodiscard]] static async::Awaitable<core::Result<WebSocket>> connect(WsConnectRequest request);

  ~WebSocket();

  WebSocket(const WebSocket&) = delete;
  WebSocket& operator=(const WebSocket&) = delete;
  WebSocket(WebSocket&&) noexcept;
  WebSocket& operator=(WebSocket&&) noexcept;

  /// Await the next complete message, suspending on socket readiness. Returns
  /// `std::nullopt` when `timeout` elapses first (a partially received
  /// message is kept and continues accumulating on the next call). Control
  /// frames are transparent: libcurl answers pings itself and pongs are
  /// swallowed. A peer close frame surfaces once as `WsMessageKind::close`;
  /// receiving after that (or after `close`) is an error.
  [[nodiscard]] async::Awaitable<core::Result<std::optional<WsMessage>>> receive(std::chrono::milliseconds timeout);

  /// Send one complete text message, suspending on socket writability for
  /// partial sends.
  [[nodiscard]] async::Awaitable<core::Result<void>> send_text(std::string payload);

  /// Send a close frame with `code` and `reason`, then complete the closing
  /// handshake by draining inbound frames until the peer's close echo or EOF
  /// (bounded, so an unresponsive peer cannot hang teardown). This guarantees
  /// the close frame is delivered before the connection is torn down on
  /// destruction. After it returns the connection no longer accepts sends or
  /// receives; the TCP/TLS teardown itself is synchronous and happens on
  /// destruction.
  [[nodiscard]] async::Awaitable<core::Result<void>> close(std::uint16_t code, std::string reason);

private:
  struct Impl;

  explicit WebSocket(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::http

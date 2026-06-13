// include/oran/channel-qq/gateway_transport.hpp — QQ gateway WebSocket driver.
//
// Drives the pure `GatewaySession` state machine over `http::WebSocket`,
// returning one non-lifecycle dispatch per `next_dispatch()` resume. This is
// the network half of the QQ port: persistent connection ownership, heartbeat
// scheduling, token injection for Identify/Resume, and reconnect backoff live
// here, while `QqChannel` owns translation into `InboundMessage`.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>

#include <oran/channel-qq/gateway.hpp>

namespace orangutan::channel::qq {

class TokenStore;

struct GatewayTransportOptions {
  /// URL returned by `GET /gateway/bot`.
  std::string gateway_url;
  std::chrono::milliseconds handshake_timeout{30'000};
  /// Bounds waits before Hello and idle reads after Hello. Once Hello arms the
  /// heartbeat, this timeout no longer terminates `next_dispatch()`; it only
  /// gives cancellation and liveness checks a finite cadence.
  std::chrono::milliseconds frame_timeout{60'000};
  /// openclaw parity: [1s, 2s, 5s, 10s, 3s, 60s], clamped at the end.
  std::array<std::chrono::milliseconds, 6> reconnect_delays{
      std::chrono::milliseconds{1'000},
      std::chrono::milliseconds{2'000},
      std::chrono::milliseconds{5'000},
      std::chrono::milliseconds{10'000},
      std::chrono::milliseconds{3'000},
      std::chrono::milliseconds{60'000},
  };
  std::size_t max_reconnect_attempts{100};
  GatewaySessionOptions session{};
};

/// Caller-owned gateway driver. Not thread-safe: one coroutine owns the driver
/// and calls `next_dispatch()` in the same one-resume-per-call shape the
/// `Channel` trait expects.
class GatewayTransport {
public:
  GatewayTransport(TokenStore& tokens, GatewayTransportOptions options);
  ~GatewayTransport();

  GatewayTransport(const GatewayTransport&) = delete;
  GatewayTransport& operator=(const GatewayTransport&) = delete;
  GatewayTransport(GatewayTransport&&) noexcept;
  GatewayTransport& operator=(GatewayTransport&&) noexcept;

  /// Return the next non-lifecycle dispatch. Lifecycle frames are handled
  /// internally: Hello sends Identify/Resume, heartbeat timers send op-1, and
  /// reconnect/close signals re-dial with the configured backoff.
  [[nodiscard]] async::Awaitable<core::Result<GatewayDispatch>> next_dispatch();

  /// Gracefully close the current socket, if connected. After close, further
  /// `next_dispatch()` calls fail with `conflict`.
  [[nodiscard]] async::Awaitable<core::Result<void>> close(std::uint16_t code = 1000, std::string reason = "shutdown");

  /// Session continuity, for later persistence by the adapter.
  void restore_session(std::string session_id, std::uint32_t last_seq);
  [[nodiscard]] std::string_view session_id() const noexcept;
  [[nodiscard]] std::uint32_t last_seq() const noexcept;
  [[nodiscard]] bool can_resume() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::channel::qq

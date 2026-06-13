// src/oran-channel-qq/gateway_transport.cpp — QQ gateway WebSocket driver.

#include <oran/channel-qq/gateway_transport.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <utility>

#include <asio/experimental/awaitable_operators.hpp>
#include <asio/this_coro.hpp>

#include <oran/async/sleep.hpp>
#include <oran/core/error.hpp>
#include <oran/http/websocket.hpp>

#include <oran/channel-qq/token_store.hpp>

namespace orangutan::channel::qq {

namespace {

using namespace std::chrono_literals;

using orangutan::core::Error;

[[nodiscard]] core::Result<void> validate_options(const GatewayTransportOptions& options) {
  if (options.gateway_url.empty()) {
    return std::unexpected(Error::invalid_argument("qq gateway url must be non-empty"));
  }
  if (options.handshake_timeout.count() <= 0) {
    return std::unexpected(Error::invalid_argument("qq gateway handshake timeout must be positive"));
  }
  if (options.frame_timeout.count() <= 0) {
    return std::unexpected(Error::invalid_argument("qq gateway frame timeout must be positive"));
  }
  if (options.max_reconnect_attempts == 0) {
    return std::unexpected(Error::invalid_argument("qq gateway max reconnect attempts must be positive"));
  }
  return {};
}

[[nodiscard]] std::chrono::milliseconds reconnect_delay(const GatewayTransportOptions& options,
                                                        std::size_t attempt) noexcept {
  const auto index = std::min(attempt, options.reconnect_delays.size() - 1);
  return options.reconnect_delays[index];
}

[[nodiscard]] bool is_retryable_transport_error(core::ErrorKind kind) noexcept {
  return kind == core::ErrorKind::network || kind == core::ErrorKind::timeout || kind == core::ErrorKind::upstream;
}

[[nodiscard]] core::Error close_recovery_error(std::uint16_t close_code, CloseRecovery recovery) {
  switch (recovery) {
    case CloseRecovery::fix_config:
      return Error::config("qq gateway closed because configuration is invalid")
          .with("close_code", std::to_string(close_code));
    case CloseRecovery::fatal:
      return Error{core::ErrorKind::permission_denied, "qq gateway account is offline or banned"}.with(
          "close_code",
          std::to_string(close_code));
    case CloseRecovery::resume:
    case CloseRecovery::fresh_identify:
    case CloseRecovery::refresh_token:
      return Error::network("qq gateway closed").with("close_code", std::to_string(close_code));
  }
  return Error::internal("unhandled qq gateway close recovery");
}

}  // namespace

struct GatewayTransport::Impl {
  struct ReactionOutcome {
    std::optional<GatewayDispatch> dispatch;
    GatewayReaction::Reconnect reconnect{GatewayReaction::Reconnect::none};
  };

  Impl(TokenStore& token_store, GatewayTransportOptions opts)
      : tokens{&token_store}, options{std::move(opts)}, session{options.session} {}

  TokenStore* tokens;
  GatewayTransportOptions options;
  GatewaySession session;
  std::optional<http::WebSocket> socket;
  std::optional<std::chrono::steady_clock::time_point> next_heartbeat;
  std::size_t reconnect_attempts{0};
  bool closed{false};

  [[nodiscard]] core::Result<void> ensure_open() const {
    if (closed) {
      return std::unexpected(Error{core::ErrorKind::conflict, "qq gateway transport is closed"});
    }
    return {};
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> connect() {
    if (auto valid = validate_options(options); !valid) {
      co_return std::unexpected(std::move(valid).error());
    }
    auto request = http::WsConnectRequest{};
    request.url = options.gateway_url;
    request.headers.push_back(http::Header{.name = "User-Agent", .value = "orangutan/qq-channel"});
    request.handshake_timeout = options.handshake_timeout;
    auto connected = co_await http::WebSocket::connect(std::move(request));
    if (!connected) {
      co_return std::unexpected(std::move(connected).error().with("stage", "qq_gateway_connect"));
    }
    socket.emplace(std::move(*connected));
    next_heartbeat.reset();
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> reconnect_after(std::string_view reason) {
    socket.reset();
    next_heartbeat.reset();
    if (reconnect_attempts >= options.max_reconnect_attempts) {
      co_return std::unexpected(
          Error::network("qq gateway reconnect attempts exhausted").with("reason", std::string{reason}));
    }
    const auto delay = reconnect_delay(options, reconnect_attempts);
    ++reconnect_attempts;
    auto executor = co_await asio::this_coro::executor;
    auto slept = co_await async::sleep_for(executor, delay);
    if (!slept) {
      co_return std::unexpected(std::move(slept).error().with("stage", "qq_gateway_reconnect_backoff"));
    }
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> send_identify_or_resume(GatewayReaction::Send send) {
    if (socket == std::nullopt) {
      co_return std::unexpected(Error{core::ErrorKind::conflict, "qq gateway socket is not connected"});
    }
    if (send == GatewayReaction::Send::heartbeat) {
      auto sent = co_await socket->send_text(session.build_heartbeat());
      if (!sent) {
        co_return std::unexpected(std::move(sent).error().with("stage", "qq_gateway_send_heartbeat"));
      }
      next_heartbeat = std::chrono::steady_clock::now() + session.heartbeat_interval();
      co_return core::Result<void>{};
    }
    if (send == GatewayReaction::Send::none) {
      co_return core::Result<void>{};
    }

    auto token = co_await tokens->access_token(std::chrono::steady_clock::now());
    if (!token) {
      co_return std::unexpected(std::move(token).error().with("stage", "qq_gateway_token"));
    }

    auto payload =
        send == GatewayReaction::Send::resume ? session.build_resume(*token) : session.build_identify(*token);
    auto sent = co_await socket->send_text(std::move(payload));
    if (!sent) {
      co_return std::unexpected(std::move(sent).error().with("stage", "qq_gateway_send_auth"));
    }
    co_return core::Result<void>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> send_due_heartbeat() {
    if (!next_heartbeat) {
      co_return core::Result<void>{};
    }
    if (std::chrono::steady_clock::now() < *next_heartbeat) {
      co_return core::Result<void>{};
    }
    co_return co_await send_identify_or_resume(GatewayReaction::Send::heartbeat);
  }

  [[nodiscard]] async::Awaitable<core::Result<std::optional<http::WsMessage>>> receive_or_heartbeat() {
    if (socket == std::nullopt) {
      co_return std::unexpected(Error{core::ErrorKind::conflict, "qq gateway socket is not connected"});
    }
    if (!next_heartbeat) {
      co_return co_await socket->receive(options.frame_timeout);
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= *next_heartbeat) {
      if (auto sent = co_await send_due_heartbeat(); !sent) {
        co_return std::unexpected(std::move(sent).error());
      }
      co_return std::optional<http::WsMessage>{};
    }

    auto executor = co_await asio::this_coro::executor;
    const auto heartbeat_wait = *next_heartbeat - now;
    using namespace asio::experimental::awaitable_operators;
    auto raced = co_await (socket->receive(options.frame_timeout) || async::sleep_for(executor, heartbeat_wait));
    if (raced.index() == 0) {
      co_return std::get<0>(std::move(raced));
    }

    auto slept = std::get<1>(std::move(raced));
    if (!slept) {
      co_return std::unexpected(std::move(slept).error().with("stage", "qq_gateway_heartbeat_timer"));
    }
    if (auto sent = co_await send_due_heartbeat(); !sent) {
      co_return std::unexpected(std::move(sent).error());
    }
    co_return std::optional<http::WsMessage>{};
  }

  [[nodiscard]] async::Awaitable<core::Result<ReactionOutcome>> handle_reaction(GatewayReaction reaction) {
    auto outcome = ReactionOutcome{};
    if (reaction.heartbeat_interval) {
      next_heartbeat = std::chrono::steady_clock::now() + *reaction.heartbeat_interval;
    }

    if (auto sent = co_await send_identify_or_resume(reaction.send); !sent) {
      co_return std::unexpected(std::move(sent).error());
    }

    if (reaction.session_ready) {
      reconnect_attempts = 0;
    }
    if (reaction.dispatch) {
      reconnect_attempts = 0;
      outcome.dispatch = std::move(*reaction.dispatch);
      co_return outcome;
    }
    if (reaction.reconnect == GatewayReaction::Reconnect::fresh) {
      session.reset();
      outcome.reconnect = GatewayReaction::Reconnect::fresh;
      co_return outcome;
    }
    if (reaction.reconnect == GatewayReaction::Reconnect::resume) {
      outcome.reconnect = GatewayReaction::Reconnect::resume;
      co_return outcome;
    }
    co_return outcome;
  }

  [[nodiscard]] async::Awaitable<core::Result<void>> handle_peer_close(const http::WsMessage& message) {
    const auto recovery = classify_close_code(message.close_code);
    switch (recovery) {
      case CloseRecovery::resume:
        co_return co_await reconnect_after("close_resume");
      case CloseRecovery::fresh_identify:
        session.reset();
        co_return co_await reconnect_after("close_fresh");
      case CloseRecovery::refresh_token:
        tokens->invalidate();
        session.reset();
        co_return co_await reconnect_after("close_refresh_token");
      case CloseRecovery::fix_config:
      case CloseRecovery::fatal:
        co_return std::unexpected(close_recovery_error(message.close_code, recovery));
    }
    co_return std::unexpected(Error::internal("unhandled qq gateway close recovery"));
  }
};

GatewayTransport::GatewayTransport(TokenStore& tokens, GatewayTransportOptions options)
    : impl_{std::make_unique<Impl>(tokens, std::move(options))} {}

GatewayTransport::~GatewayTransport() = default;

GatewayTransport::GatewayTransport(GatewayTransport&&) noexcept = default;

GatewayTransport& GatewayTransport::operator=(GatewayTransport&&) noexcept = default;

async::Awaitable<core::Result<GatewayDispatch>> GatewayTransport::next_dispatch() {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (impl_ == nullptr) {
    co_return std::unexpected(core::Error::invalid_argument("qq gateway transport has been moved from"));
  }
  if (auto open = impl_->ensure_open(); !open) {
    co_return std::unexpected(std::move(open).error());
  }

  for (;;) {
    if (impl_->socket == std::nullopt) {
      auto connected = co_await impl_->connect();
      if (!connected) {
        if (!is_retryable_transport_error(connected.error().kind())) {
          co_return std::unexpected(std::move(connected).error());
        }
        if (auto slept = co_await impl_->reconnect_after("connect_failed"); !slept) {
          co_return std::unexpected(std::move(slept).error());
        }
        continue;
      }
    }

    auto received = co_await impl_->receive_or_heartbeat();
    if (!received) {
      if (received.error().kind() == core::ErrorKind::cancelled) {
        co_return std::unexpected(std::move(received).error());
      }
      if (!is_retryable_transport_error(received.error().kind()) &&
          received.error().kind() != core::ErrorKind::conflict) {
        co_return std::unexpected(std::move(received).error());
      }
      if (auto slept = co_await impl_->reconnect_after("receive_failed"); !slept) {
        co_return std::unexpected(std::move(slept).error());
      }
      continue;
    }

    if (!*received) {
      if (!impl_->next_heartbeat) {
        co_return std::unexpected(Error{core::ErrorKind::timeout, "qq gateway timed out waiting for Hello"});
      }
      continue;
    }

    auto message = std::move(**received);
    if (message.kind == http::WsMessageKind::close) {
      if (auto handled = co_await impl_->handle_peer_close(message); !handled) {
        co_return std::unexpected(std::move(handled).error());
      }
      continue;
    }
    if (message.kind != http::WsMessageKind::text) {
      co_return std::unexpected(Error::upstream("qq gateway sent a non-text frame"));
    }

    auto reaction = impl_->session.consume(message.payload);
    if (!reaction) {
      co_return std::unexpected(std::move(reaction).error().with("stage", "qq_gateway_consume"));
    }
    auto handled = co_await impl_->handle_reaction(std::move(*reaction));
    if (!handled) {
      if (!is_retryable_transport_error(handled.error().kind()) &&
          handled.error().kind() != core::ErrorKind::conflict) {
        co_return std::unexpected(std::move(handled).error());
      }
      if (auto slept = co_await impl_->reconnect_after("server_requested_reconnect"); !slept) {
        co_return std::unexpected(std::move(slept).error());
      }
      continue;
    }
    if (handled->reconnect != GatewayReaction::Reconnect::none) {
      if (auto slept = co_await impl_->reconnect_after("server_requested_reconnect"); !slept) {
        co_return std::unexpected(std::move(slept).error());
      }
      continue;
    }
    if (handled->dispatch) {
      co_return std::move(*handled->dispatch);
    }
  }
}

async::Awaitable<core::Result<void>> GatewayTransport::close(std::uint16_t code, std::string reason) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (impl_ == nullptr) {
    co_return std::unexpected(core::Error::invalid_argument("qq gateway transport has been moved from"));
  }
  impl_->closed = true;
  impl_->next_heartbeat.reset();
  if (impl_->socket == std::nullopt) {
    co_return core::Result<void>{};
  }
  auto socket = std::move(*impl_->socket);
  impl_->socket.reset();
  co_return co_await socket.close(code, std::move(reason));
}

void GatewayTransport::restore_session(std::string session_id, std::uint32_t last_seq) {
  impl_->session.restore(std::move(session_id), last_seq);
}

std::string_view GatewayTransport::session_id() const noexcept {
  return impl_->session.session_id();
}

std::uint32_t GatewayTransport::last_seq() const noexcept {
  return impl_->session.last_seq();
}

bool GatewayTransport::can_resume() const noexcept {
  return impl_->session.can_resume();
}

}  // namespace orangutan::channel::qq

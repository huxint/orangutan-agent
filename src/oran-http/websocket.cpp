// src/oran-http/websocket.cpp — libcurl-backed WebSocket connection.
//
// Uses libcurl's connect-only WebSocket mode (CURLOPT_CONNECT_ONLY = 2): the
// handshake runs as a normal curl transfer driven by non-blocking
// `curl_multi_perform` rounds between cancel-aware timer ticks; after it
// completes, `curl_ws_recv` / `curl_ws_send` operate the socket in
// non-blocking mode (they return CURLE_AGAIN instead of blocking, so calling
// them on an io worker does not violate A7). When curl reports CURLE_AGAIN the
// coroutine suspends on asio socket readiness — a dup of curl's active socket
// wrapped in `asio::posix::stream_descriptor` — raced against the caller's
// deadline, so an idle connection costs no thread.

#include <oran/http/websocket.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <unistd.h>

#include <asio/as_tuple.hpp>
#include <asio/error.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <curl/curl.h>
#include <curl/websockets.h>

#include <oran/async/sleep.hpp>
#include <oran/core/error.hpp>

#include "_impl/curl_common.hpp"

namespace orangutan::http {
namespace {

using namespace std::chrono_literals;

using orangutan::core::Error;

using detail::curl_error;
using detail::curl_global;
using detail::curl_multi_error;
using detail::CurlEasy;
using detail::CurlEasyRegistration;
using detail::CurlHeaders;
using detail::CurlMulti;
using detail::is_cancelled;

/// Handshake progress is polled on a short timer tick instead of a blocking
/// `curl_multi_poll`: the dial happens rarely (connect + reconnects), and the
/// tick keeps the coroutine cancel-aware without occupying a CPU-pool thread.
constexpr auto kHandshakeTick = 10ms;

/// One `curl_ws_recv` chunk. Larger messages arrive across several chunks and
/// are reassembled; gateway frames are far smaller than this.
constexpr std::size_t kRecvChunkBytes = std::size_t{16} * 1024;

/// Sends have no caller-supplied deadline (cancellation bounds them), so the
/// writability wait re-checks cancellation on this interval.
constexpr auto kSendWaitTick = 1s;

/// After sending a close frame, the closing handshake drains inbound frames
/// until the peer's close echo / EOF. This bounds the wait so a peer that
/// never acknowledges cannot hang teardown; on loopback the EOF is immediate.
constexpr auto kCloseDrainTimeout = 5s;

/// RFC 6455: a close frame payload is at most 125 bytes, 2 of which carry the
/// status code.
constexpr std::size_t kMaxCloseReasonBytes = 123;

[[nodiscard]] bool is_ws_url(std::string_view url) noexcept {
  return url.starts_with("ws://") || url.starts_with("wss://");
}

[[nodiscard]] core::Result<void> validate_request(const WsConnectRequest& request) {
  if (request.url.empty()) {
    return std::unexpected(Error::invalid_argument("websocket url must be non-empty"));
  }
  if (!is_ws_url(request.url)) {
    return std::unexpected(Error::invalid_argument("websocket url must use ws or wss").with("url", request.url));
  }
  if (request.handshake_timeout.count() <= 0) {
    return std::unexpected(Error::invalid_argument("websocket handshake timeout must be positive"));
  }
  if (request.handshake_timeout.count() > std::numeric_limits<long>::max()) {
    return std::unexpected(Error::invalid_argument("websocket handshake timeout is too large"));
  }
  return {};
}

/// Suspend until curl's socket is ready for `what` or `timeout` elapses.
/// Returns true on readiness, false on timeout; cancellation surfaces as
/// `Error::cancelled`. The descriptor wraps a dup of the curl socket so its
/// destructor never closes the connection's fd.
[[nodiscard]] async::Awaitable<core::Result<bool>> wait_socket(curl_socket_t socket,
                                                               asio::posix::stream_descriptor::wait_type what,
                                                               std::chrono::steady_clock::duration timeout) {
  co_await asio::this_coro::throw_if_cancelled(false);
  auto executor = co_await asio::this_coro::executor;

  const int watched_fd = ::dup(static_cast<int>(socket));
  if (watched_fd < 0) {
    co_return std::unexpected(Error::internal("failed to duplicate websocket socket for readiness wait"));
  }
  auto descriptor = asio::posix::stream_descriptor{executor, watched_fd};

  using namespace asio::experimental::awaitable_operators;
  auto raced = co_await (descriptor.async_wait(what, asio::as_tuple(asio::use_awaitable)) ||
                         async::sleep_for(executor, timeout));
  if (raced.index() == 0) {
    const auto [ec] = std::get<0>(raced);
    if (ec == asio::error::operation_aborted) {
      co_return std::unexpected(Error::cancelled());
    }
    if (ec) {
      co_return std::unexpected(Error::network("websocket socket wait failed").with("asio_error", ec.message()));
    }
    co_return true;
  }
  if (auto slept = std::get<1>(raced); !slept) {
    co_return std::unexpected(std::move(slept).error());
  }
  co_return false;
}

/// Drive the upgrade handshake to completion with cancel checks and an
/// overall deadline between non-blocking progress rounds.
[[nodiscard]] async::Awaitable<core::Result<void>>
drive_handshake(CURLM* multi, CURL* easy, std::chrono::milliseconds timeout) {
  auto executor = co_await asio::this_coro::executor;
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  int running = 0;
  for (;;) {
    if (is_cancelled(cancellation)) {
      co_return std::unexpected(Error::cancelled());
    }
    if (auto code = curl_multi_perform(multi, &running); code != CURLM_OK) {
      co_return std::unexpected(curl_multi_error(code, "ws_handshake"));
    }
    if (running == 0) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      co_return std::unexpected(Error{core::ErrorKind::timeout, "websocket handshake timed out"});
    }
    if (auto slept = co_await async::sleep_for(executor, kHandshakeTick); !slept) {
      co_return std::unexpected(std::move(slept).error());
    }
  }

  CURLMsg* message = nullptr;
  int queued = 0;
  while ((message = curl_multi_info_read(multi, &queued)) != nullptr) {
    if (message->msg == CURLMSG_DONE && message->easy_handle == easy) {
      if (message->data.result != CURLE_OK) {
        co_return std::unexpected(curl_error(message->data.result, "ws_handshake"));
      }
      co_return core::Result<void>{};
    }
  }
  co_return std::unexpected(Error::network("websocket handshake finished without curl completion message"));
}

[[nodiscard]] std::uint16_t decode_close_code(std::string_view payload) noexcept {
  if (payload.size() < 2) {
    return 0;
  }
  return static_cast<std::uint16_t>((static_cast<std::uint8_t>(payload[0]) << 8) |
                                    static_cast<std::uint8_t>(payload[1]));
}

[[nodiscard]] async::Awaitable<core::Result<void>>
send_frame(CURL* easy,
           curl_socket_t socket,
           std::string_view payload,
           unsigned int kind_flags,
           const asio::cancellation_state& cancellation,
           std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

}  // namespace

struct WebSocket::Impl {
  Impl(CurlMulti owning_multi,
       CurlEasy connected_easy,
       std::unique_ptr<CurlHeaders> handshake_headers,
       curl_socket_t active_socket)
      : multi{std::move(owning_multi)}, easy{std::move(connected_easy)}, headers{std::move(handshake_headers)},
        socket{active_socket}, registration{multi.get(), easy.get()} {}

  // In libcurl's connect-only mode the live connection is parked in the multi
  // handle's connection pool, and `curl_ws_recv` / `curl_ws_send` only relocate
  // it while the easy handle stays attached to that multi. So the connection
  // owns both handles for its whole lifetime: the easy handle is never removed
  // until destruction, and the multi must outlive it.
  CurlMulti multi;
  CurlEasy easy;
  // CURLOPT_HTTPHEADER borrows the slist, so it outlives the connection.
  std::unique_ptr<CurlHeaders> headers;
  curl_socket_t socket{CURL_SOCKET_BAD};
  bool peer_closed{false};
  bool locally_closed{false};

  // A message that timed out mid-reassembly stays here and continues
  // accumulating on the next `receive`.
  std::string partial_payload;
  std::optional<WsMessageKind> partial_kind;

  std::array<char, kRecvChunkBytes> chunk{};

  // Declared last so it destructs first: the easy handle leaves the multi
  // before either handle is cleaned up (curl_easy_cleanup on a still-attached
  // handle is undefined).
  CurlEasyRegistration registration;

  [[nodiscard]] core::Result<void> ensure_open() const {
    if (peer_closed || locally_closed) {
      return std::unexpected(Error{core::ErrorKind::conflict, "websocket is already closed"});
    }
    return {};
  }
};

async::Awaitable<core::Result<WebSocket>> WebSocket::connect(WsConnectRequest request) {
  co_await asio::this_coro::throw_if_cancelled(false);
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (is_cancelled(cancellation)) {
    co_return std::unexpected(Error::cancelled());
  }
  if (!curl_global().ok()) {
    co_return std::unexpected(Error::internal("curl global initialization failed"));
  }
  if (auto valid = validate_request(request); !valid) {
    co_return std::unexpected(std::move(valid).error());
  }

  auto easy = CurlEasy{};
  if (easy.get() == nullptr) {
    co_return std::unexpected(Error::internal("failed to allocate curl easy handle"));
  }
  auto headers = std::make_unique<CurlHeaders>();
  for (const auto& header : request.headers) {
    if (auto appended = headers->append(header.name, header.value); !appended) {
      co_return std::unexpected(std::move(appended).error());
    }
  }

  if (curl_easy_setopt(easy.get(), CURLOPT_URL, request.url.c_str()) != CURLE_OK ||
      curl_easy_setopt(easy.get(), CURLOPT_CONNECT_ONLY, 2L) != CURLE_OK ||
      curl_easy_setopt(easy.get(), CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
      curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
      curl_easy_setopt(easy.get(), CURLOPT_HTTPHEADER, headers->get()) != CURLE_OK ||
      curl_easy_setopt(easy.get(), CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.handshake_timeout.count())) !=
          CURLE_OK) {
    co_return std::unexpected(Error::internal("failed to configure curl websocket handle"));
  }
#if defined(CURLWS_NOAUTOPONG)
  // Newer libcurl releases can leave an automatic pong pending while the
  // caller waits for more readable data. Own the control-frame write so the
  // receive loop can wait for writability and preserve its overall deadline.
  if (curl_easy_setopt(easy.get(), CURLOPT_WS_OPTIONS, CURLWS_NOAUTOPONG) != CURLE_OK) {
    co_return std::unexpected(Error::internal("failed to configure curl websocket pong handling"));
  }
#endif

  auto multi = CurlMulti{};
  if (multi.get() == nullptr) {
    co_return std::unexpected(Error::internal("failed to allocate curl multi handle"));
  }
  if (auto added = curl_multi_add_handle(multi.get(), easy.get()); added != CURLM_OK) {
    co_return std::unexpected(curl_multi_error(added, "ws_add_handle"));
  }
  // The easy handle stays attached to the multi for the connection's whole
  // lifetime (see Impl): on any failure below we detach it explicitly, but on
  // success ownership of both handles moves into the Impl still attached.
  if (auto performed = co_await drive_handshake(multi.get(), easy.get(), request.handshake_timeout); !performed) {
    curl_multi_remove_handle(multi.get(), easy.get());
    co_return std::unexpected(std::move(performed).error());
  }

  curl_socket_t socket = CURL_SOCKET_BAD;
  if (curl_easy_getinfo(easy.get(), CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK || socket == CURL_SOCKET_BAD) {
    curl_multi_remove_handle(multi.get(), easy.get());
    co_return std::unexpected(Error::network("websocket handshake produced no active socket"));
  }

  co_return WebSocket{std::make_unique<Impl>(std::move(multi), std::move(easy), std::move(headers), socket)};
}

WebSocket::WebSocket(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}

WebSocket::~WebSocket() = default;

WebSocket::WebSocket(WebSocket&&) noexcept = default;

WebSocket& WebSocket::operator=(WebSocket&&) noexcept = default;

async::Awaitable<core::Result<std::optional<WsMessage>>> WebSocket::receive(std::chrono::milliseconds timeout) {
  co_await asio::this_coro::throw_if_cancelled(false);
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (impl_ == nullptr) {
    co_return std::unexpected(Error::invalid_argument("websocket has been moved from"));
  }
  if (auto open = impl_->ensure_open(); !open) {
    co_return std::unexpected(std::move(open).error());
  }
  if (timeout.count() <= 0) {
    co_return std::unexpected(Error::invalid_argument("websocket receive timeout must be positive"));
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (is_cancelled(cancellation)) {
      co_return std::unexpected(Error::cancelled());
    }

    std::size_t received = 0;
    const curl_ws_frame* meta = nullptr;
    const auto code = curl_ws_recv(impl_->easy.get(), impl_->chunk.data(), impl_->chunk.size(), &received, &meta);

    if (code == CURLE_AGAIN) {
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) {
        co_return std::optional<WsMessage>{};
      }
      auto readable = co_await wait_socket(impl_->socket, asio::posix::stream_descriptor::wait_read, remaining);
      if (!readable) {
        co_return std::unexpected(std::move(readable).error());
      }
      if (!*readable) {
        co_return std::optional<WsMessage>{};
      }
      continue;
    }
    if (code == CURLE_GOT_NOTHING) {
      impl_->peer_closed = true;
      co_return std::unexpected(Error::network("websocket connection closed by peer without close frame"));
    }
    if (code != CURLE_OK) {
      co_return std::unexpected(curl_error(code, "ws_recv"));
    }
    if (meta == nullptr) {
      co_return std::unexpected(Error::internal("curl websocket receive returned no frame metadata"));
    }

    const auto payload = std::string_view{impl_->chunk.data(), received};
    if ((meta->flags & CURLWS_PING) != 0) {
#if defined(CURLWS_NOAUTOPONG)
      auto pong = co_await send_frame(impl_->easy.get(), impl_->socket, payload, CURLWS_PONG, cancellation, deadline);
      if (!pong) {
        co_return std::unexpected(std::move(pong).error());
      }
#endif
      // Older libcurl releases answer pings automatically. In either mode the
      // control frame stays transparent to the caller.
      continue;
    }
    if ((meta->flags & CURLWS_PONG) != 0) {
      // Pong payloads carry nothing the protocol layer needs.
      continue;
    }
    if ((meta->flags & CURLWS_CLOSE) != 0) {
      // RFC 6455 forbids fragmented control frames and caps them at 125
      // bytes, so a close frame always arrives in one chunk.
      impl_->peer_closed = true;
      auto message = WsMessage{.kind = WsMessageKind::close, .payload = {}, .close_code = decode_close_code(payload)};
      if (payload.size() > 2) {
        message.payload.assign(payload.substr(2));
      }
      co_return std::optional<WsMessage>{std::move(message)};
    }

    if (!impl_->partial_kind.has_value()) {
      impl_->partial_kind = (meta->flags & CURLWS_BINARY) != 0 ? WsMessageKind::binary : WsMessageKind::text;
    }
    impl_->partial_payload.append(payload);

    const bool frame_complete = meta->bytesleft == 0;
    const bool message_complete = frame_complete && (meta->flags & CURLWS_CONT) == 0;
    if (message_complete) {
      auto message = WsMessage{.kind = *impl_->partial_kind, .payload = std::move(impl_->partial_payload)};
      impl_->partial_payload = {};
      impl_->partial_kind.reset();
      co_return std::optional<WsMessage>{std::move(message)};
    }
    // More chunks of this message follow — drain whatever curl already
    // buffered before suspending on the socket again.
  }
}

namespace {

/// Send one complete frame of `kind_flags`, suspending on writability for
/// CURLE_AGAIN / partial sends. Per curl_ws_send's contract, retries pass the
/// remaining payload with the same type flag and curl continues the frame.
[[nodiscard]] async::Awaitable<core::Result<void>>
send_frame(CURL* easy,
           curl_socket_t socket,
           std::string_view payload,
           unsigned int kind_flags,
           const asio::cancellation_state& cancellation,
           std::optional<std::chrono::steady_clock::time_point> deadline) {
  std::size_t offset = 0;
  for (;;) {
    if (is_cancelled(cancellation)) {
      co_return std::unexpected(Error::cancelled());
    }
    auto wait_duration = std::chrono::steady_clock::duration{kSendWaitTick};
    if (deadline.has_value()) {
      const auto remaining = *deadline - std::chrono::steady_clock::now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) {
        co_return std::unexpected(Error{core::ErrorKind::timeout, "websocket frame send timed out"});
      }
      if (remaining < wait_duration) {
        wait_duration = remaining;
      }
    }

    std::size_t sent = 0;
    const auto code = curl_ws_send(easy, payload.data() + offset, payload.size() - offset, &sent, 0, kind_flags);
    if (code == CURLE_OK) {
      offset += sent;
      if (offset >= payload.size()) {
        co_return core::Result<void>{};
      }
    } else if (code == CURLE_AGAIN) {
      offset += sent;
    } else {
      co_return std::unexpected(curl_error(code, "ws_send"));
    }

    auto writable = co_await wait_socket(socket, asio::posix::stream_descriptor::wait_write, wait_duration);
    if (!writable) {
      co_return std::unexpected(std::move(writable).error());
    }
    // A writability timeout just re-checks cancellation and retries: sends
    // carry no caller deadline, cancellation is the bound.
  }
}

/// Complete the closing handshake after our close frame is sent: drain inbound
/// frames until the peer's close echo or EOF, bounded by `kCloseDrainTimeout`.
/// Reading keeps the connection open until the peer acknowledges, so our close
/// frame is actually delivered before the connection is torn down on
/// destruction (an abortive `curl_easy_cleanup` would otherwise drop it). The
/// drained payloads are discarded — the connection is already locally closed.
[[nodiscard]] async::Awaitable<void> drain_until_peer_closed(CURL* easy,
                                                             curl_socket_t socket,
                                                             std::span<char> chunk,
                                                             const asio::cancellation_state& cancellation) {
  const auto deadline = std::chrono::steady_clock::now() + kCloseDrainTimeout;
  for (;;) {
    if (is_cancelled(cancellation) || std::chrono::steady_clock::now() >= deadline) {
      co_return;
    }

    std::size_t received = 0;
    const curl_ws_frame* meta = nullptr;
    const auto code = curl_ws_recv(easy, chunk.data(), chunk.size(), &received, &meta);

    if (code == CURLE_AGAIN) {
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) {
        co_return;
      }
      auto readable = co_await wait_socket(socket, asio::posix::stream_descriptor::wait_read, remaining);
      if (!readable || !*readable) {
        co_return;  // cancelled or timed out — we did our part by sending close
      }
      continue;
    }
    // EOF, the peer's close echo, or any transport error all mean the peer is
    // done; either way the close frame has been delivered.
    if (code != CURLE_OK || meta == nullptr || (meta->flags & CURLWS_CLOSE) != 0) {
      co_return;
    }
    // Otherwise discard the data and keep draining.
  }
}

}  // namespace

async::Awaitable<core::Result<void>> WebSocket::send_text(std::string payload) {
  co_await asio::this_coro::throw_if_cancelled(false);
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (impl_ == nullptr) {
    co_return std::unexpected(Error::invalid_argument("websocket has been moved from"));
  }
  if (auto open = impl_->ensure_open(); !open) {
    co_return std::unexpected(std::move(open).error());
  }
  co_return co_await send_frame(impl_->easy.get(), impl_->socket, payload, CURLWS_TEXT, cancellation);
}

async::Awaitable<core::Result<void>> WebSocket::close(std::uint16_t code, std::string reason) {
  co_await asio::this_coro::throw_if_cancelled(false);
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (impl_ == nullptr) {
    co_return std::unexpected(Error::invalid_argument("websocket has been moved from"));
  }
  if (impl_->locally_closed) {
    co_return std::unexpected(Error{core::ErrorKind::conflict, "websocket is already closed"});
  }
  if (reason.size() > kMaxCloseReasonBytes) {
    co_return std::unexpected(Error::invalid_argument("websocket close reason exceeds 123 bytes"));
  }

  auto payload = std::string{};
  payload.reserve(2 + reason.size());
  payload.push_back(static_cast<char>(code >> 8));
  payload.push_back(static_cast<char>(code & 0xff));
  payload.append(reason);

  // Mark closed before the write: a half-sent close frame still leaves the
  // connection unusable for follow-up traffic.
  impl_->locally_closed = true;
  if (auto sent = co_await send_frame(impl_->easy.get(), impl_->socket, payload, CURLWS_CLOSE, cancellation); !sent) {
    co_return std::unexpected(std::move(sent).error());
  }

  // Complete the closing handshake so the close frame is delivered before the
  // connection is torn down on destruction (see drain_until_peer_closed).
  co_await drain_until_peer_closed(impl_->easy.get(), impl_->socket, impl_->chunk, cancellation);
  co_return core::Result<void>{};
}

}  // namespace orangutan::http

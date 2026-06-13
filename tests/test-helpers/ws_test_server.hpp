// tests/test-helpers/ws_test_server.hpp — scripted loopback WebSocket server.
//
// Serves one scripted action list per accepted connection: it completes the
// RFC 6455 upgrade handshake (computing the real Sec-WebSocket-Accept so
// libcurl's validation passes), then plays its actions in order — sending
// text/binary/fragmented/ping/close frames, pausing, or reading and recording
// one client frame. Recorded handshake requests and client frames let tests
// assert exact wire behavior. Test-only: std::jthread and blocking sockets
// are allowed here (rules/async-and-concurrency.md A3 exception).

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

namespace orangutan::tests::ws {

// --- scripted actions ------------------------------------------------------

struct SendText {
  std::string payload;
};

struct SendBinary {
  std::string payload;
};

/// One logical text message split into WebSocket continuation fragments.
struct SendFragmentedText {
  std::vector<std::string> fragments;
};

struct SendClose {
  std::uint16_t code{1000};
  std::string reason;
};

struct SendPing {
  std::string payload;
};

/// Read one frame from the client and record it (see `recorded_frames`).
struct RecvFrame {};

struct Delay {
  std::chrono::milliseconds duration{0};
};

/// Answer the upgrade request with a plain HTTP error instead of 101.
struct RejectHandshake {
  int status{404};
};

using WsAction =
    std::variant<SendText, SendBinary, SendFragmentedText, SendClose, SendPing, RecvFrame, Delay, RejectHandshake>;

/// One client frame as decoded off the wire (payload already unmasked).
struct RecordedFrame {
  std::uint8_t opcode{0};  // 0x1 text, 0x2 binary, 0x8 close, 0x9 ping, 0xA pong
  std::string payload;
};

// --- handshake crypto (test-fixture quality, not a security surface) -------

[[nodiscard]] inline std::array<std::uint8_t, 20> sha1(std::string_view input) {
  std::array<std::uint32_t, 5> h{0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};

  std::string msg{input};
  const std::uint64_t bit_length = static_cast<std::uint64_t>(msg.size()) * 8U;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) {
    msg.push_back('\0');
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    msg.push_back(static_cast<char>((bit_length >> static_cast<unsigned>(shift)) & 0xffU));
  }

  for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    std::array<std::uint32_t, 80> w{};
    for (std::size_t i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(msg[chunk + 4 * i])) << 24) |
             (static_cast<std::uint32_t>(static_cast<std::uint8_t>(msg[chunk + 4 * i + 1])) << 16) |
             (static_cast<std::uint32_t>(static_cast<std::uint8_t>(msg[chunk + 4 * i + 2])) << 8) |
             static_cast<std::uint32_t>(static_cast<std::uint8_t>(msg[chunk + 4 * i + 3]));
    }
    for (std::size_t i = 16; i < 80; ++i) {
      w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    auto [a, b, c, d, e] = h;
    for (std::size_t i = 0; i < 80; ++i) {
      std::uint32_t f = 0;
      std::uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999U;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1U;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCU;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6U;
      }
      const std::uint32_t temp = std::rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = std::rotl(b, 30);
      b = a;
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  std::array<std::uint8_t, 20> digest{};
  for (std::size_t i = 0; i < 5; ++i) {
    digest[4 * i] = static_cast<std::uint8_t>(h[i] >> 24);
    digest[4 * i + 1] = static_cast<std::uint8_t>(h[i] >> 16);
    digest[4 * i + 2] = static_cast<std::uint8_t>(h[i] >> 8);
    digest[4 * i + 3] = static_cast<std::uint8_t>(h[i]);
  }
  return digest;
}

[[nodiscard]] inline std::string base64(std::span<const std::uint8_t> bytes) {
  static constexpr std::string_view kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                 (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                 static_cast<std::uint32_t>(bytes[i + 2]);
    out.push_back(kTable[(triple >> 18) & 0x3fU]);
    out.push_back(kTable[(triple >> 12) & 0x3fU]);
    out.push_back(kTable[(triple >> 6) & 0x3fU]);
    out.push_back(kTable[triple & 0x3fU]);
  }
  if (const auto rest = bytes.size() - i; rest == 1) {
    const std::uint32_t triple = static_cast<std::uint32_t>(bytes[i]) << 16;
    out.push_back(kTable[(triple >> 18) & 0x3fU]);
    out.push_back(kTable[(triple >> 12) & 0x3fU]);
    out.append("==");
  } else if (rest == 2) {
    const std::uint32_t triple =
        (static_cast<std::uint32_t>(bytes[i]) << 16) | (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
    out.push_back(kTable[(triple >> 18) & 0x3fU]);
    out.push_back(kTable[(triple >> 12) & 0x3fU]);
    out.push_back(kTable[(triple >> 6) & 0x3fU]);
    out.push_back('=');
  }
  return out;
}

[[nodiscard]] inline std::string websocket_accept(std::string_view key) {
  static constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const auto digest = sha1(std::string{key} + std::string{kGuid});
  return base64(digest);
}

// --- the server -------------------------------------------------------------

class ScriptedWsServer {
public:
  /// One action list per connection, served in accept order.
  explicit ScriptedWsServer(std::vector<std::vector<WsAction>> connections) : connections_{std::move(connections)} {
    asio::ip::tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1"), 0};
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(static_cast<int>(connections_.size()) + 1);
    port_ = acceptor_.local_endpoint().port();
    worker_ = std::jthread{[this] { serve(); }};
  }

  ~ScriptedWsServer() {
    stop_.store(true);
    if (accepted_count() < connections_.size() && port_ != 0) {
      // Unblock the accept loop so the worker can observe `stop_`.
      std::error_code ignored;
      asio::io_context poke_io;
      asio::ip::tcp::socket poke{poke_io};
      static_cast<void>(poke.connect(asio::ip::tcp::endpoint{asio::ip::make_address("127.0.0.1"), port_}, ignored));
      static_cast<void>(poke.close(ignored));
    }
    std::error_code ignored;
    static_cast<void>(acceptor_.close(ignored));
  }

  ScriptedWsServer(const ScriptedWsServer&) = delete;
  ScriptedWsServer& operator=(const ScriptedWsServer&) = delete;

  [[nodiscard]] std::string url(std::string_view path = "/gateway") const {
    return "ws://127.0.0.1:" + std::to_string(port_) + std::string{path};
  }

  [[nodiscard]] std::size_t accepted_count() const {
    const std::scoped_lock lock{mutex_};
    return handshakes_.size();
  }

  /// Raw upgrade request text (request line + headers) by accept order.
  [[nodiscard]] std::string handshake_request(std::size_t index) const {
    const std::scoped_lock lock{mutex_};
    return index < handshakes_.size() ? handshakes_[index] : std::string{};
  }

  /// Client frames recorded by `RecvFrame` actions, across all connections.
  [[nodiscard]] std::vector<RecordedFrame> recorded_frames() const {
    const std::scoped_lock lock{mutex_};
    return frames_;
  }

private:
  /// RAII env-var override; named distinctly from the HTTP test servers'
  /// `ScopedEnv` so one TU can include both helpers.
  class ScopedEnvVar {
  public:
    ScopedEnvVar(std::string name, const std::string& value) : name_{std::move(name)} {
      if (const auto* old = std::getenv(name_.c_str()); old != nullptr) {
        old_value_ = old;
      }
      setenv(name_.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvVar() {
      if (old_value_) {
        setenv(name_.c_str(), old_value_->c_str(), 1);
      } else {
        unsetenv(name_.c_str());
      }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

  private:
    std::string name_;
    std::optional<std::string> old_value_;
  };

  void serve() {
    for (const auto& script : connections_) {
      std::error_code ec;
      auto socket = acceptor_.accept(ec);
      if (ec || stop_.load()) {
        return;
      }
      serve_connection(socket, script);
      std::error_code ignored;
      static_cast<void>(socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored));
    }
  }

  void serve_connection(asio::ip::tcp::socket& socket, const std::vector<WsAction>& script) {
    auto request = read_handshake(socket);
    if (request.empty()) {
      return;
    }
    {
      const std::scoped_lock lock{mutex_};
      handshakes_.push_back(request);
    }

    if (!script.empty() && std::holds_alternative<RejectHandshake>(script.front())) {
      const auto status = std::get<RejectHandshake>(script.front()).status;
      write_all(socket,
                "HTTP/1.1 " + std::to_string(status) + " Scripted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
      return;
    }

    const auto key = header_value(request, "sec-websocket-key");
    write_all(
        socket,
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
            websocket_accept(key) + "\r\n\r\n");

    for (const auto& action : script) {
      const bool keep_going = std::visit([&](const auto& act) { return run(socket, act); }, action);
      if (!keep_going || stop_.load()) {
        return;
      }
    }
  }

  bool run(asio::ip::tcp::socket& socket, const SendText& action) {
    return write_all(socket, frame(0x1, action.payload, true));
  }

  bool run(asio::ip::tcp::socket& socket, const SendBinary& action) {
    return write_all(socket, frame(0x2, action.payload, true));
  }

  bool run(asio::ip::tcp::socket& socket, const SendFragmentedText& action) {
    for (std::size_t i = 0; i < action.fragments.size(); ++i) {
      const auto opcode = static_cast<std::uint8_t>(i == 0 ? 0x1 : 0x0);
      const bool fin = i + 1 == action.fragments.size();
      if (!write_all(socket, frame(opcode, action.fragments[i], fin))) {
        return false;
      }
    }
    return true;
  }

  bool run(asio::ip::tcp::socket& socket, const SendClose& action) {
    std::string payload;
    payload.push_back(static_cast<char>(action.code >> 8));
    payload.push_back(static_cast<char>(action.code & 0xffU));
    payload.append(action.reason);
    return write_all(socket, frame(0x8, payload, true));
  }

  bool run(asio::ip::tcp::socket& socket, const SendPing& action) {
    return write_all(socket, frame(0x9, action.payload, true));
  }

  bool run(asio::ip::tcp::socket& socket, const RecvFrame&) {
    auto decoded = read_frame(socket);
    if (!decoded) {
      return false;
    }
    const std::scoped_lock lock{mutex_};
    frames_.push_back(std::move(*decoded));
    return true;
  }

  bool run(asio::ip::tcp::socket&, const Delay& action) {
    std::this_thread::sleep_for(action.duration);
    return true;
  }

  bool run(asio::ip::tcp::socket&, const RejectHandshake&) {
    return false;  // only meaningful as the first action; handled in serve_connection
  }

  [[nodiscard]] static std::string read_handshake(asio::ip::tcp::socket& socket) {
    std::string request;
    std::array<char, 4096> buffer{};
    std::error_code ec;
    while (!request.contains("\r\n\r\n")) {
      const auto read = socket.read_some(asio::buffer(buffer), ec);
      if (read > 0) {
        request.append(buffer.data(), read);
      }
      if (ec) {
        return {};
      }
    }
    return request;
  }

  [[nodiscard]] static std::string header_value(std::string_view request, std::string_view lowercase_name) {
    for (const auto line : std::views::split(request, std::string_view{"\r\n"})) {
      const std::string_view text{line};
      const auto colon = text.find(':');
      if (colon == std::string_view::npos || colon != lowercase_name.size()) {
        continue;
      }
      const auto matches = std::ranges::equal(text.substr(0, colon), lowercase_name, [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
      });
      if (!matches) {
        continue;
      }
      auto value = text.substr(colon + 1);
      while (value.starts_with(' ') || value.starts_with('\t')) {
        value.remove_prefix(1);
      }
      return std::string{value};
    }
    return {};
  }

  /// Encode one server frame (servers never mask).
  [[nodiscard]] static std::string frame(std::uint8_t opcode, std::string_view payload, bool fin) {
    std::string out;
    out.push_back(static_cast<char>((fin ? 0x80U : 0x00U) | opcode));
    if (payload.size() < 126) {
      out.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xffffU) {
      out.push_back(static_cast<char>(126));
      out.push_back(static_cast<char>(payload.size() >> 8));
      out.push_back(static_cast<char>(payload.size() & 0xffU));
    } else {
      out.push_back(static_cast<char>(127));
      for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(
            static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> static_cast<unsigned>(shift)) & 0xffU));
      }
    }
    out.append(payload);
    return out;
  }

  /// Decode one client frame, unmasking the payload (clients always mask).
  [[nodiscard]] static std::optional<RecordedFrame> read_frame(asio::ip::tcp::socket& socket) {
    std::array<std::uint8_t, 8> header{};
    if (!read_exact(socket, std::span{header}.first(2))) {
      return std::nullopt;
    }
    const auto opcode = static_cast<std::uint8_t>(header[0] & 0x0fU);
    const bool masked = (header[1] & 0x80U) != 0;
    std::uint64_t length = header[1] & 0x7fU;
    if (length == 126) {
      if (!read_exact(socket, std::span{header}.first(2))) {
        return std::nullopt;
      }
      length = (static_cast<std::uint64_t>(header[0]) << 8) | header[1];
    } else if (length == 127) {
      if (!read_exact(socket, std::span{header})) {
        return std::nullopt;
      }
      length = 0;
      for (const auto byte : header) {
        length = (length << 8) | byte;
      }
    }

    std::array<std::uint8_t, 4> mask{};
    if (masked && !read_exact(socket, std::span{mask})) {
      return std::nullopt;
    }

    std::string payload(length, '\0');
    if (length > 0 && !read_exact(socket, std::as_writable_bytes(std::span{payload}))) {
      return std::nullopt;
    }
    if (masked) {
      for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4]);
      }
    }
    return RecordedFrame{.opcode = opcode, .payload = std::move(payload)};
  }

  template <typename SpanLike>
  [[nodiscard]] static bool read_exact(asio::ip::tcp::socket& socket, SpanLike target) {
    std::error_code ec;
    asio::read(socket, asio::buffer(target.data(), target.size()), ec);
    return !ec;
  }

  static bool write_all(asio::ip::tcp::socket& socket, std::string_view bytes) {
    std::error_code ec;
    asio::write(socket, asio::buffer(bytes), ec);
    return !ec;
  }

  asio::io_context io_;
  asio::ip::tcp::acceptor acceptor_{io_};
  std::uint16_t port_{0};
  std::vector<std::vector<WsAction>> connections_;
  // Loopback traffic must bypass any configured proxy (the ws:// dial rides
  // libcurl's HTTP machinery, which honors proxy env vars).
  ScopedEnvVar no_proxy_upper_{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnvVar no_proxy_lower_{"no_proxy", "127.0.0.1,localhost"};
  mutable std::mutex mutex_;
  std::vector<std::string> handshakes_;
  std::vector<RecordedFrame> frames_;
  std::atomic_bool stop_{false};
  std::jthread worker_;
};

}  // namespace orangutan::tests::ws

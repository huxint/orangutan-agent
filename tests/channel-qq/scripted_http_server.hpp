// tests/channel-qq/scripted_http_server.hpp — sequential scripted-response server.
//
// Serves one scripted response per accepted connection (the http client opens
// a fresh connection per send) and records each raw request so tests can
// assert exact request lines, headers, and JSON bodies. Responses can carry a
// pre-write delay to exercise singleflight and cancellation paths.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

namespace orangutan::tests {

/// RAII env var override (same shape as the `oran-http` test-local helper).
class ScopedEnv {
public:
  ScopedEnv(std::string name, const std::string& value) : name_{std::move(name)} {
    if (const auto* old = std::getenv(name_.c_str()); old != nullptr) {
      old_value_ = old;
    }
    setenv(name_.c_str(), value.c_str(), 1);
  }

  ~ScopedEnv() {
    if (old_value_) {
      setenv(name_.c_str(), old_value_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

struct ScriptedResponse {
  int status{200};
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::chrono::milliseconds delay{0};
};

class ScriptedHttpServer {
public:
  explicit ScriptedHttpServer(std::vector<ScriptedResponse> responses) : responses_{std::move(responses)} {
    asio::ip::tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1"), 0};
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(static_cast<int>(responses_.size()) + 1);
    port_ = acceptor_.local_endpoint().port();
    worker_ = std::jthread{[this] { serve(); }};
  }

  ~ScriptedHttpServer() {
    stop_.store(true);
    if (served_count() < responses_.size() && port_ != 0) {
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

  ScriptedHttpServer(const ScriptedHttpServer&) = delete;
  ScriptedHttpServer& operator=(const ScriptedHttpServer&) = delete;

  /// `http://127.0.0.1:<port>` — pass as `ApiClientOptions::base_url`.
  [[nodiscard]] std::string base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  [[nodiscard]] std::string url(std::string_view path) const {
    return base_url() + std::string{path};
  }

  [[nodiscard]] std::size_t served_count() const {
    const std::scoped_lock lock{mutex_};
    return requests_.size();
  }

  /// Raw request text (request line + headers + body) by serve order.
  [[nodiscard]] std::string request_text(std::size_t index) const {
    const std::scoped_lock lock{mutex_};
    return index < requests_.size() ? requests_[index] : std::string{};
  }

private:
  void serve() {
    for (const auto& response : responses_) {
      std::error_code ec;
      auto socket = acceptor_.accept(ec);
      if (ec || stop_.load()) {
        return;
      }

      auto request = read_request(socket);
      if (request.empty()) {
        return;
      }
      {
        const std::scoped_lock lock{mutex_};
        requests_.push_back(std::move(request));
      }

      if (response.delay > std::chrono::milliseconds{0}) {
        std::this_thread::sleep_for(response.delay);
      }

      auto payload = render(response);
      asio::write(socket, asio::buffer(payload), ec);
      std::error_code ignored;
      static_cast<void>(socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored));
    }
  }

  [[nodiscard]] static std::string read_request(asio::ip::tcp::socket& socket) {
    std::string request;
    std::array<char, 4096> buffer{};
    std::error_code ec;
    for (;;) {
      const auto read = socket.read_some(asio::buffer(buffer), ec);
      if (read > 0) {
        request.append(buffer.data(), read);
      }
      if (ec) {
        return ec == asio::error::eof ? request : std::string{};
      }
      const auto header_end = request.find("\r\n\r\n");
      if (header_end == std::string::npos) {
        continue;
      }
      const auto body_bytes = request.size() - (header_end + 4);
      if (body_bytes >= content_length(std::string_view{request}.substr(0, header_end))) {
        return request;
      }
    }
  }

  [[nodiscard]] static std::size_t content_length(std::string_view headers) {
    for (const auto line : std::views::split(headers, std::string_view{"\r\n"})) {
      const std::string_view text{line};
      static constexpr std::string_view kName = "content-length:";
      if (text.size() <= kName.size()) {
        continue;
      }
      auto matches = std::ranges::equal(text.substr(0, kName.size()), kName, [](unsigned char a, unsigned char b) {
        return std::tolower(a) == b;
      });
      if (!matches) {
        continue;
      }
      auto value = text.substr(kName.size());
      while (value.starts_with(' ')) {
        value.remove_prefix(1);
      }
      std::size_t length = 0;
      if (std::from_chars(value.data(), value.data() + value.size(), length).ec == std::errc{}) {
        return length;
      }
    }
    return 0;
  }

  [[nodiscard]] static std::string render(const ScriptedResponse& response) {
    auto payload = "HTTP/1.1 " + std::to_string(response.status) + " Scripted\r\n";
    for (const auto& [name, value] : response.headers) {
      payload += name + ": " + value + "\r\n";
    }
    payload += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    payload += "Connection: close\r\n\r\n";
    payload += response.body;
    return payload;
  }

  asio::io_context io_;
  asio::ip::tcp::acceptor acceptor_{io_};
  std::uint16_t port_{0};
  std::vector<ScriptedResponse> responses_;
  // Loopback traffic must bypass any configured HTTP proxy; curl's NO_PROXY
  // matching does not understand wildcard entries like `127.*`.
  ScopedEnv no_proxy_upper_{"NO_PROXY", "127.0.0.1,localhost"};
  ScopedEnv no_proxy_lower_{"no_proxy", "127.0.0.1,localhost"};
  mutable std::mutex mutex_;
  std::vector<std::string> requests_;
  std::atomic_bool stop_{false};
  std::jthread worker_;
};

}  // namespace orangutan::tests

// tests/http/test_client.cpp - libcurl body client coverage.

#include <oran/http.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/thread_pool.hpp>
#include <asio/write.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>

#include "../test-helpers/run_async.hpp"

using namespace std::chrono_literals;

namespace {

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace http = orangutan::http;
namespace test = orangutan::tests;
using asio::ip::tcp;

class OneShotHttpServer {
public:
  explicit OneShotHttpServer(std::string response) : response_{std::move(response)} {
    tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1"), 0};
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(1);
    port_ = acceptor_.local_endpoint().port();
    worker_ = std::jthread{[this] { serve(); }};
  }

  ~OneShotHttpServer() {
    std::error_code ignored;
    acceptor_.close(ignored);
  }

  [[nodiscard]] std::string url(std::string_view path = "/") const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string{path};
  }

  [[nodiscard]] std::string request_text() const {
    return request_;
  }

  [[nodiscard]] bool served() const noexcept {
    return served_.load();
  }

private:
  void serve() {
    std::error_code ec;
    auto socket = acceptor_.accept(ec);
    if (ec) {
      return;
    }

    std::array<char, 4096> buffer{};
    for (;;) {
      const auto read = socket.read_some(asio::buffer(buffer), ec);
      if (ec && ec != asio::error::eof) {
        return;
      }
      if (read > 0) {
        request_.append(buffer.data(), read);
      }
      if (request_.contains("\r\n\r\n")) {
        if (!request_.contains("Content-Length:") || request_.ends_with("hello")) {
          break;
        }
      }
      if (ec == asio::error::eof) {
        break;
      }
    }

    asio::write(socket, asio::buffer(response_), ec);
    served_ = !ec;
  }

  asio::io_context io_;
  tcp::acceptor acceptor_{io_};
  std::uint16_t port_{0};
  std::string response_;
  std::string request_;
  std::atomic_bool served_{false};
  std::jthread worker_;
};

}  // namespace

TEST_CASE("Client sends a body request and collects the response", "[unit][http][client]") {
  OneShotHttpServer server{"HTTP/1.1 201 Created\r\n"
                           "Content-Type: application/json\r\n"
                           "X-Trace: abc\r\n"
                           "Content-Length: 11\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "{\"ok\":true}"};
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};

  test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
    auto request = http::BodyRequest{};
    request.method = "POST";
    request.url = server.url("/v1/messages");
    request.headers.push_back(http::Header{.name = "Authorization", .value = "Bearer test"});
    request.body = "hello";
    request.timeout = 2s;

    auto response = co_await client.send(std::move(request));

    REQUIRE(response.has_value());
    REQUIRE(response->status_code == 201);
    REQUIRE(response->body == R"({"ok":true})");
    REQUIRE(response->headers.size() >= 2);
  });

  REQUIRE(server.served());
  REQUIRE(server.request_text().starts_with("POST /v1/messages HTTP/1.1"));
  REQUIRE(server.request_text().contains("Authorization: Bearer test"));
  REQUIRE(server.request_text().contains("hello"));
  blocking.join();
}

TEST_CASE("Client validates request shape before curl dispatch", "[unit][http][client]") {
  asio::thread_pool blocking{1};
  auto client = http::Client{blocking.get_executor()};

  SECTION("missing url") {
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await client.send(http::BodyRequest{
          .method = "GET",
          .url = "",
          .headers = {},
          .body = {},
          .timeout = 2s,
      });
      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::invalid_argument);
    });
  }

  SECTION("unsupported scheme") {
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await client.send(http::BodyRequest{
          .method = "GET",
          .url = "file:///tmp/nope",
          .headers = {},
          .body = {},
          .timeout = 2s,
      });
      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::invalid_argument);
    });
  }

  SECTION("unsupported method") {
    test::run_async([&](asio::io_context&) -> async::Awaitable<void> {
      auto response = co_await client.send(http::BodyRequest{
          .method = "TRACE",
          .url = "http://127.0.0.1/",
          .headers = {},
          .body = {},
          .timeout = 2s,
      });
      REQUIRE_FALSE(response.has_value());
      REQUIRE(response.error().kind() == core::ErrorKind::invalid_argument);
    });
  }

  blocking.join();
}

TEST_CASE("Client observes cancellation before dispatch", "[unit][http][client]") {
  asio::io_context io;
  asio::thread_pool blocking{1};
  asio::cancellation_signal signal;
  auto client = http::Client{blocking.get_executor()};
  std::optional<core::Result<http::BodyResponse>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<http::BodyResponse>> {
        co_await asio::this_coro::throw_if_cancelled(false);
        co_return co_await client.send(http::BodyRequest{
            .method = "GET",
            .url = "http://127.0.0.1/",
            .headers = {},
            .body = {},
            .timeout = 10s,
        });
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<http::BodyResponse> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  blocking.join();
}

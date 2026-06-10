// src/oran-channel-qq/api_client.cpp — QQ API request/retry/normalize implementation.

#include <oran/channel-qq/api_client.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <asio/this_coro.hpp>

#include <nlohmann/json.hpp>

#include <oran/async/sleep.hpp>
#include <oran/core/error.hpp>

namespace orangutan::channel::qq {

namespace {

[[nodiscard]] bool is_absolute_url(std::string_view url) noexcept {
  return url.starts_with("https://") || url.starts_with("http://");
}

[[nodiscard]] bool iequals(std::string_view lhs, std::string_view rhs) noexcept {
  return std::ranges::equal(lhs, rhs, [](unsigned char a, unsigned char b) {
    return std::tolower(a) == std::tolower(b);
  });
}

[[nodiscard]] std::string_view header_value(const http::BodyResponse& response, std::string_view name) noexcept {
  const auto match =
      std::ranges::find_if(response.headers, [&](const http::Header& header) { return iequals(header.name, name); });
  return match == response.headers.end() ? std::string_view{} : std::string_view{match->value};
}

/// `retry-after` arrives as decimal seconds (possibly fractional). Anything
/// unparseable or non-positive falls back to the configured default.
[[nodiscard]] std::chrono::milliseconds parse_retry_after_delay(std::string_view retry_after,
                                                                std::chrono::milliseconds fallback) noexcept {
  double seconds = 0.0;
  const auto parsed = std::from_chars(retry_after.data(), retry_after.data() + retry_after.size(), seconds);
  if (parsed.ec == std::errc{} && std::isfinite(seconds) && seconds > 0.0) {
    return std::chrono::milliseconds{static_cast<std::int64_t>(seconds * 1000.0)};
  }
  return fallback;
}

[[nodiscard]] bool is_retryable_gateway_status(int status) noexcept {
  return status == 502 || status == 503 || status == 504;
}

/// Status-class mapping mirrors `oran-provider`'s transport mapping. Context
/// carries the platform's diagnostics (trace id, business envelope) but never
/// raw bodies.
[[nodiscard]] core::Error api_error(std::string_view method,
                                    std::string_view path,
                                    const ApiResponse& response,
                                    std::chrono::milliseconds rate_limit_fallback) {
  auto error = [&]() -> core::Error {
    if (response.http_status == 401 || response.http_status == 403) {
      return core::Error{core::ErrorKind::auth, "qq api authentication failed"};
    }
    if (response.http_status == 408) {
      return core::Error{core::ErrorKind::timeout, "qq api request timed out"};
    }
    if (response.http_status == 429) {
      return core::Error::rate_limit("qq api rate limited")
          .with_retry_after(parse_retry_after_delay(response.retry_after, rate_limit_fallback));
    }
    if (response.http_status == 404) {
      return core::Error::not_found("qq api target not found");
    }
    if (response.http_status >= 500) {
      return core::Error::upstream("qq api upstream error");
    }
    if (response.http_status >= 400) {
      return core::Error::invalid_argument("qq api rejected request");
    }
    return core::Error::upstream("qq api business error");
  }();

  error.with("method", std::string{method})
      .with("path", std::string{path})
      .with("http_status", std::to_string(response.http_status));
  if (!response.trace_id.empty()) {
    error.with("trace_id", response.trace_id);
  }
  if (response.biz_code != 0) {
    error.with("biz_code", std::to_string(response.biz_code));
    if (!response.biz_message.empty()) {
      error.with("biz_message", response.biz_message);
    }
  }
  return error;
}

}  // namespace

ApiResponse normalize_api_response(http::BodyResponse response) {
  auto normalized = ApiResponse{
      .http_status = response.status_code,
      .body = std::move(response.body),
      .trace_id = std::string{header_value(response, "x-tps-trace-id")},
      .retry_after = std::string{header_value(response, "retry-after")},
      .biz_code = 0,
      .biz_message = {},
  };

  if (normalized.body.empty()) {
    return normalized;
  }
  const auto payload = nlohmann::json::parse(normalized.body, nullptr, /*allow_exceptions=*/false);
  if (!payload.is_object()) {
    return normalized;
  }
  if (const auto code = payload.find("code"); code != payload.end() && code->is_number_integer()) {
    normalized.biz_code = code->get<int>();
    if (const auto message = payload.find("message"); message != payload.end() && message->is_string()) {
      normalized.biz_message = message->get<std::string>();
    }
  }
  return normalized;
}

ApiClient::ApiClient(const http::Client& client, TokenStore& tokens, ApiClientOptions options)
    : client_{&client}, tokens_{&tokens}, options_{std::move(options)} {}

async::Awaitable<core::Result<ApiResponse>> ApiClient::get(std::string path) {
  return request("GET", std::move(path), std::nullopt);
}

async::Awaitable<core::Result<ApiResponse>> ApiClient::post(std::string path, std::string body_json) {
  return request("POST", std::move(path), std::move(body_json));
}

async::Awaitable<core::Result<ApiResponse>> ApiClient::put(std::string path, std::string body_json) {
  return request("PUT", std::move(path), std::move(body_json));
}

async::Awaitable<core::Result<ApiResponse>> ApiClient::del(std::string path) {
  return request("DELETE", std::move(path), std::nullopt);
}

async::Awaitable<core::Result<ApiResponse>>
ApiClient::request(std::string method, std::string path, std::optional<std::string> body_json) {
  int token_refresh_retries = 0;
  int rate_limit_retries = 0;
  int gateway_retries = 0;

  for (;;) {
    auto token = co_await tokens_->access_token(std::chrono::steady_clock::now());
    if (!token) {
      co_return std::unexpected(std::move(token).error().with("method", method).with("path", path));
    }

    auto request = http::BodyRequest{
        .method = method,
        .url = is_absolute_url(path) ? path : options_.base_url + path,
        .headers = {{.name = "Content-Type", .value = "application/json"},
                    {.name = "User-Agent", .value = options_.user_agent},
                    {.name = "Authorization", .value = "QQBot " + *token}},
        // Legacy parity: non-GET requests always carry a JSON body.
        .body = method == "GET" ? std::string{} : body_json.value_or("{}"),
        .timeout = options_.request_timeout,
    };

    auto sent = co_await client_->send(std::move(request));
    if (!sent) {
      co_return std::unexpected(std::move(sent).error().with("method", method).with("path", path));
    }
    auto response = normalize_api_response(*std::move(sent));

    if (response.http_status == 401 && token_refresh_retries < options_.max_token_refresh_retries) {
      ++token_refresh_retries;
      tokens_->invalidate();
      continue;
    }

    if (response.http_status == 429 && rate_limit_retries < options_.max_rate_limit_retries) {
      ++rate_limit_retries;
      const auto delay = parse_retry_after_delay(response.retry_after, options_.rate_limit_fallback_delay);
      auto executor = co_await asio::this_coro::executor;
      auto slept = co_await async::sleep_for(executor, delay);
      if (!slept) {
        co_return std::unexpected(std::move(slept).error().with("method", method).with("path", path));
      }
      continue;
    }

    if (is_retryable_gateway_status(response.http_status) && gateway_retries < options_.max_gateway_retries) {
      const auto delay = options_.gateway_retry_base_delay * (1 << gateway_retries);
      ++gateway_retries;
      auto executor = co_await asio::this_coro::executor;
      auto slept = co_await async::sleep_for(executor, delay);
      if (!slept) {
        co_return std::unexpected(std::move(slept).error().with("method", method).with("path", path));
      }
      continue;
    }

    if (response.http_status >= 400 || response.biz_code != 0) {
      co_return std::unexpected(api_error(method, path, response, options_.rate_limit_fallback_delay));
    }

    co_return response;
  }
}

}  // namespace orangutan::channel::qq

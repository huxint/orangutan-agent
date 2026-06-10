// src/oran-channel-qq/token_store.cpp — QQ app-access-token refresh implementation.

#include <oran/channel-qq/token_store.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <nlohmann/json.hpp>

#include <oran/core/error.hpp>
#include <oran/http/client.hpp>

namespace orangutan::channel::qq {

namespace {

/// One in-flight token-endpoint refresh. The leader publishes its outcome and
/// wakes every waiter; waiters return that shared outcome instead of issuing
/// their own request.
struct RefreshFlight {
  std::optional<core::Result<std::string>> outcome;
  std::vector<std::shared_ptr<asio::steady_timer>> waiters;
};

[[nodiscard]] std::chrono::seconds parse_expires_in(const nlohmann::json& payload, const TokenStoreOptions& options) {
  auto ttl = options.fallback_ttl;
  if (const auto entry = payload.find("expires_in"); entry != payload.end()) {
    if (entry->is_number_integer()) {
      ttl = std::chrono::seconds{entry->get<std::int64_t>()};
    } else if (entry->is_string()) {
      // The endpoint has been observed returning the TTL as a decimal string.
      const auto text = entry->get<std::string_view>();
      std::int64_t parsed = 0;
      if (std::from_chars(text.data(), text.data() + text.size(), parsed).ec == std::errc{}) {
        ttl = std::chrono::seconds{parsed};
      }
    }
  }
  return std::max(ttl, options.min_ttl);
}

/// Error mapping for the credential endpoint. Context stays non-secret: the
/// status and the platform's business code/message, never request or response
/// bodies (the latter can carry a token on partial success).
[[nodiscard]] core::Error token_endpoint_error(const http::BodyResponse& response) {
  const auto payload = nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
  auto error = [&]() -> core::Error {
    if (response.status_code == 429) {
      return core::Error::rate_limit("qq token endpoint rate limited");
    }
    if (response.status_code >= 500) {
      return core::Error::upstream("qq token endpoint upstream error");
    }
    return core::Error{core::ErrorKind::auth, "qq token endpoint rejected credentials"};
  }();
  error.with("http_status", std::to_string(response.status_code));
  if (payload.is_object()) {
    if (const auto code = payload.find("code"); code != payload.end() && code->is_number_integer()) {
      error.with("biz_code", std::to_string(code->get<std::int64_t>()));
    }
    if (const auto message = payload.find("message"); message != payload.end() && message->is_string()) {
      error.with("biz_message", message->get<std::string>());
    }
  }
  return error;
}

}  // namespace

struct TokenStore::Impl {
  const http::Client* client;
  Credentials credentials;
  TokenStoreOptions options;

  std::string token;
  std::chrono::steady_clock::time_point expiry{std::chrono::steady_clock::time_point::min()};
  std::shared_ptr<RefreshFlight> flight;
  std::uint64_t refresh_count{0};

  [[nodiscard]] bool fresh_at(std::chrono::steady_clock::time_point now) const {
    return !token.empty() && now + options.refresh_ahead < expiry;
  }

  [[nodiscard]] async::Awaitable<core::Result<std::string>> refresh(std::chrono::steady_clock::time_point now) {
    const auto body = nlohmann::json{
        {"appId", credentials.app_id},
        {"clientSecret", credentials.client_secret},
    };
    auto request = http::BodyRequest{
        .method = "POST",
        .url = options.token_url,
        .headers = {{.name = "Content-Type", .value = "application/json"},
                    {.name = "User-Agent", .value = "orangutan/qq-channel"}},
        .body = body.dump(),
        .timeout = options.request_timeout,
    };

    auto sent = co_await client->send(std::move(request));
    if (!sent) {
      co_return std::unexpected(std::move(sent).error().with("stage", "qq_token_refresh"));
    }
    if (sent->status_code < 200 || sent->status_code >= 300) {
      co_return std::unexpected(token_endpoint_error(*sent));
    }

    const auto payload = nlohmann::json::parse(sent->body, nullptr, /*allow_exceptions=*/false);
    if (!payload.is_object()) {
      co_return std::unexpected(core::Error::parsing("qq token response is not a JSON object"));
    }
    const auto access_token = payload.find("access_token");
    if (access_token == payload.end() || !access_token->is_string()) {
      co_return std::unexpected(token_endpoint_error(*sent).with("reason", "missing_access_token"));
    }

    token = access_token->get<std::string>();
    expiry = now + parse_expires_in(payload, options);
    ++refresh_count;
    co_return token;
  }
};

TokenStore::TokenStore(const http::Client& client, Credentials credentials, TokenStoreOptions options)
    : impl_{std::make_unique<Impl>(&client, std::move(credentials), std::move(options))} {}

TokenStore::~TokenStore() = default;

async::Awaitable<core::Result<std::string>> TokenStore::access_token(std::chrono::steady_clock::time_point now) {
  for (;;) {
    if (impl_->fresh_at(now)) {
      co_return impl_->token;
    }

    if (impl_->flight == nullptr) {
      break;  // No refresh in flight — become the leader below.
    }

    // Join the in-flight refresh and await its shared outcome. The leader
    // wakes us by rescheduling the timer, which also completes with
    // `operation_aborted` — own-cancellation is distinguished afterwards.
    auto flight = impl_->flight;
    auto executor = co_await asio::this_coro::executor;
    auto waiter = std::make_shared<asio::steady_timer>(executor);
    waiter->expires_at(std::chrono::steady_clock::time_point::max());
    flight->waiters.push_back(waiter);

    asio::error_code ignored;
    co_await waiter->async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    const auto cancellation = co_await asio::this_coro::cancellation_state;
    if (cancellation.cancelled() != asio::cancellation_type::none) {
      co_return std::unexpected(core::Error::cancelled());
    }
    if (!flight->outcome.has_value()) {
      continue;  // Spurious wake; re-evaluate from the top.
    }
    if (flight->outcome->has_value() || (*flight->outcome).error().kind() != core::ErrorKind::cancelled) {
      co_return *flight->outcome;
    }
    // The leader was cancelled, not the endpoint failing — loop and try the
    // refresh ourselves.
  }

  auto flight = std::make_shared<RefreshFlight>();
  impl_->flight = flight;
  try {
    flight->outcome = co_await impl_->refresh(now);
  } catch (const std::exception& error) {
    flight->outcome = std::unexpected(core::Error::internal(error.what()));
  }
  impl_->flight = nullptr;

  // Wake each waiter on its own executor — basic_waitable_timer is unsafe to
  // mutate cross-executor (same publish idiom as the oran-io singleflight).
  for (const auto& waiter : flight->waiters) {
    auto executor = waiter->get_executor();
    asio::post(std::move(executor), [waiter] { waiter->expires_at(std::chrono::steady_clock::time_point::min()); });
  }

  co_return *flight->outcome;
}

void TokenStore::invalidate() {
  impl_->token.clear();
  impl_->expiry = std::chrono::steady_clock::time_point::min();
}

std::uint64_t TokenStore::refresh_count() const noexcept {
  return impl_->refresh_count;
}

}  // namespace orangutan::channel::qq

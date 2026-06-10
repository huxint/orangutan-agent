// include/oran/channel-qq/token_store.hpp — QQ bot credential/token boundary.
//
// Owns the app access token the QQ open-platform API requires, refreshing it
// through the token endpoint when the cached value is missing or close to
// expiry. Callers supply resolved credential *values*; reading them from the
// environment stays at the bootstrap credential boundary, mirroring
// `provider::resolve_adapter_credentials`.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>

namespace orangutan::http {
class Client;
}  // namespace orangutan::http

namespace orangutan::channel::qq {

/// Resolved QQ bot application credentials. The `client_secret` value must
/// never be logged or attached to error context (critical rule C5).
struct Credentials {
  std::string app_id;
  std::string client_secret;
};

struct TokenStoreOptions {
  /// QQ open-platform app-access-token endpoint.
  std::string token_url{"https://bots.qq.com/app/getAppAccessToken"};
  std::chrono::milliseconds request_timeout{30'000};
  /// Refresh when `now + refresh_ahead` reaches the cached token's expiry.
  std::chrono::seconds refresh_ahead{300};
  /// Floor applied to the endpoint's `expires_in` so a tiny TTL cannot force
  /// a refresh on every call.
  std::chrono::seconds min_ttl{60};
  /// TTL assumed when the endpoint omits `expires_in`.
  std::chrono::seconds fallback_ttl{7'200};
};

/// Caches one app access token and serializes refreshes: concurrent
/// `access_token` calls during an in-flight refresh await that request's
/// outcome instead of issuing duplicate token-endpoint calls (single flight,
/// same idiom as the `oran-io` read singleflight).
class TokenStore {
public:
  TokenStore(const http::Client& client, Credentials credentials, TokenStoreOptions options = {});
  ~TokenStore();

  TokenStore(const TokenStore&) = delete;
  TokenStore& operator=(const TokenStore&) = delete;

  /// Return a token valid past the refresh-ahead window at `now`, refreshing
  /// through the token endpoint first when needed. Cancel-aware via the
  /// underlying HTTP send and the waiter timer.
  [[nodiscard]] async::Awaitable<core::Result<std::string>> access_token(std::chrono::steady_clock::time_point now);

  /// Drop the cached token (e.g. after an authoritative 401) so the next
  /// `access_token` call refreshes. An in-flight refresh is unaffected.
  void invalidate();

  /// Completed successful refreshes — test/diagnostic observability.
  [[nodiscard]] std::uint64_t refresh_count() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::channel::qq

// include/oran/channel-qq/api_client.hpp — QQ open-platform API client.
//
// Request building, auth-header injection, retry ladder, and response
// normalization over `oran-http::Client`. Bodies cross this boundary as
// already-serialized JSON strings so the public header stays free of JSON
// parser types (critical rule C6); decoding into typed shapes is the
// adapter's job.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/http/client.hpp>

#include <oran/channel-qq/token_store.hpp>

namespace orangutan::channel::qq {

/// One normalized QQ API response. `trace_id` mirrors the platform's
/// `x-tps-trace-id` response header; `biz_code` / `biz_message` mirror the
/// `{"code": ..., "message": ...}` business-error envelope the platform can
/// return alongside any HTTP status.
struct ApiResponse {
  int http_status{0};
  std::string body;
  std::string trace_id;
  std::string retry_after;
  int biz_code{0};
  std::string biz_message;

  friend bool operator==(const ApiResponse&, const ApiResponse&) = default;
};

struct GatewaySessionStartLimit {
  std::int64_t total{0};
  std::int64_t remaining{0};
  std::chrono::milliseconds reset_after{0};
  std::int64_t max_concurrency{0};

  friend bool operator==(const GatewaySessionStartLimit&, const GatewaySessionStartLimit&) = default;
};

struct GatewayBotInfo {
  std::string url;
  std::int64_t shards{1};
  std::optional<GatewaySessionStartLimit> session_start_limit;

  friend bool operator==(const GatewayBotInfo&, const GatewayBotInfo&) = default;
};

/// Pure normalization seam: capture `x-tps-trace-id` / `retry-after` headers
/// (case-insensitively) and extract the business-error envelope from the
/// body when one is present.
[[nodiscard]] ApiResponse normalize_api_response(http::BodyResponse response);

/// Decode `GET /gateway/bot` response bodies without exposing JSON types in the
/// public header. The returned URL is suitable for `GatewayTransportOptions`.
[[nodiscard]] core::Result<GatewayBotInfo> parse_gateway_bot_response(std::string_view body);

struct ApiClientOptions {
  /// QQ open-platform API base; paths passed to the client are joined onto it
  /// unless they are already absolute URLs.
  std::string base_url{"https://api.sgroup.qq.com"};
  /// Legacy-parity default; the platform tolerates long-running calls.
  std::chrono::milliseconds request_timeout{120'000};
  std::string user_agent{"orangutan/qq-channel"};
  /// 401 → invalidate the token store and retry, at most this many times.
  int max_token_refresh_retries{1};
  /// 429 → wait the `retry-after` delay and retry, at most this many times.
  int max_rate_limit_retries{2};
  /// 502/503/504 → exponential backoff retry, at most this many times.
  int max_gateway_retries{2};
  std::chrono::milliseconds gateway_retry_base_delay{500};
  /// Delay assumed when a 429 carries no parseable `retry-after`.
  std::chrono::milliseconds rate_limit_fallback_delay{1'000};
};

/// Issues authenticated QQ API requests with the platform's retry ladder:
/// one token refresh on 401, bounded `retry-after`-honoring retries on 429,
/// and bounded exponential backoff on transient gateway statuses. All waits
/// go through `async::sleep_for`, so calls stay cancel-aware end to end.
class ApiClient {
public:
  ApiClient(const http::Client& client, TokenStore& tokens, ApiClientOptions options = {});

  ApiClient(const ApiClient&) = delete;
  ApiClient& operator=(const ApiClient&) = delete;

  [[nodiscard]] async::Awaitable<core::Result<ApiResponse>> get(std::string path);
  [[nodiscard]] async::Awaitable<core::Result<ApiResponse>> post(std::string path, std::string body_json);
  [[nodiscard]] async::Awaitable<core::Result<ApiResponse>> put(std::string path, std::string body_json);
  [[nodiscard]] async::Awaitable<core::Result<ApiResponse>> del(std::string path);

private:
  [[nodiscard]] async::Awaitable<core::Result<ApiResponse>>
  request(std::string method, std::string path, std::optional<std::string> body_json);

  const http::Client* client_;
  TokenStore* tokens_;
  ApiClientOptions options_;
};

/// Discover the current bot gateway URL through `GET /gateway/bot`.
[[nodiscard]] async::Awaitable<core::Result<GatewayBotInfo>> discover_gateway_bot(ApiClient& api);

}  // namespace orangutan::channel::qq

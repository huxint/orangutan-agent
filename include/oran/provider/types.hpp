// include/oran/provider/types.hpp — provider-domain request/response values.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <oran/core/content.hpp>
#include <oran/core/message.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/provider/cache.hpp>

namespace orangutan::provider {

struct Usage {
  std::uint64_t input_tokens{0};
  std::uint64_t output_tokens{0};
  std::uint64_t cache_creation_tokens{0};
  std::uint64_t cache_read_tokens{0};
  std::optional<double> cost_estimate;

  friend bool operator==(const Usage&, const Usage&) = default;
};

struct RetryPolicy {
  std::uint32_t max_attempts{1};
  std::chrono::milliseconds initial_backoff{0};

  friend bool operator==(const RetryPolicy&, const RetryPolicy&) = default;
};

struct Request {
  std::vector<core::Message> messages;
  std::optional<std::string> system_prompt;
  std::vector<core::ToolDef> tools;
  std::optional<std::string> tool_choice;
  std::optional<std::uint32_t> max_tokens;
  std::optional<std::uint32_t> thinking_budget;
  bool stream{true};
  std::optional<PromptCacheHints> cache;
  RetryPolicy retry{};

  friend bool operator==(const Request&, const Request&) = default;
};

struct Response {
  std::vector<core::Content> blocks;
  core::StopReason stop_reason{core::StopReason::end_turn};
  Usage usage{};
  std::optional<std::string> model_used;
  /// Profile of the `Route` target that actually produced this response.
  /// `provider::execution::Runtime` fills this with the served target's
  /// `ModelTarget::profile` on success so trace consumers can distinguish a
  /// primary response from one served by a `Route::fallbacks` entry. Concrete
  /// backends are not required to set it; the execution runtime fills it when
  /// missing so callers do not need to know which layer attributed the route.
  std::optional<std::string> route_profile_used;

  friend bool operator==(const Response&, const Response&) = default;
};

}  // namespace orangutan::provider

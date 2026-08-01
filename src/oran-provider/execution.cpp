// src/oran-provider/execution.cpp — retry/fallback execution wrapper.

#include <oran/provider/execution.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/this_coro.hpp>

#include <oran/async/sleep.hpp>
#include <oran/core/error.hpp>

namespace orangutan::provider::execution {
namespace {

class AttemptSink final : public provider::EventSink {
public:
  explicit AttemptSink(provider::EventSink* inner) noexcept : inner_{inner} {}

  [[nodiscard]] bool emitted() const noexcept {
    return emitted_;
  }

  void on_text_delta(std::string_view delta) override {
    emitted_ = true;
    inner_->on_text_delta(delta);
  }

  void on_thinking_delta(std::string_view delta) override {
    emitted_ = true;
    inner_->on_thinking_delta(delta);
  }

  void on_tool_start(std::string_view id, std::string_view name) override {
    emitted_ = true;
    inner_->on_tool_start(id, name);
  }

  void on_tool_delta(std::string_view id, std::string_view input_delta) override {
    emitted_ = true;
    inner_->on_tool_delta(id, input_delta);
  }

  void on_done(core::StopReason stop_reason) override {
    emitted_ = true;
    inner_->on_done(stop_reason);
  }

private:
  provider::EventSink* inner_;
  bool emitted_{false};
};

[[nodiscard]] core::Error invalid_retry_policy() {
  return core::Error::invalid_argument("provider retry policy requires at least one attempt")
      .with("field", "retry.max_attempts");
}

[[nodiscard]] provider::Route single_target_route(provider::ModelTarget target) {
  return provider::Route{
      .primary = std::move(target),
      .fallbacks = {},
  };
}

[[nodiscard]] std::vector<provider::ModelTarget> route_targets(const provider::Route& route) {
  std::vector<provider::ModelTarget> targets;
  targets.reserve(route.fallbacks.size() + 1);
  targets.push_back(route.primary);
  targets.insert(targets.end(), route.fallbacks.begin(), route.fallbacks.end());
  return targets;
}

[[nodiscard]] std::chrono::milliseconds retry_delay(const core::Error& error,
                                                    std::chrono::milliseconds initial_backoff) {
  if (auto retry_after = error.retry_after()) {
    return *retry_after > initial_backoff ? *retry_after : initial_backoff;
  }
  return initial_backoff;
}

[[nodiscard]] core::Error with_target_context(core::Error error,
                                              const provider::ModelTarget& target,
                                              std::uint32_t attempt,
                                              std::uint32_t max_attempts,
                                              std::string_view role) {
  return std::move(error)
      .with("provider_profile", target.profile)
      .with("provider_model", target.model)
      .with("route_role", std::string{role})
      .with("attempt", std::to_string(attempt))
      .with("max_attempts", std::to_string(max_attempts));
}

// Per-target fallback policy: a fallback target either carries its own
// thinking/cache policy or has the primary's stripped when the wire protocol
// cannot honor it (only `anthropic_messages` consumes token-budget thinking
// controls today; `openai_responses` rejects them as `invalid_request`).
// The primary attempt keeps the request verbatim — the agent loop already
// folded the primary profile's policy (and any explicit turn override) in.
void apply_target_policy(provider::Request& request, const provider::ModelTarget& target) noexcept {
  if (target.thinking_budget.has_value()) {
    request.thinking_budget = target.thinking_budget;
  } else if (target.protocol != provider::ProtocolKind::anthropic_messages) {
    request.thinking_budget = std::nullopt;
  }
  if (target.cache.has_value()) {
    if (!target.cache->enabled) {
      request.cache = std::nullopt;
    } else if (request.cache.has_value() && request.cache->prefix_bytes < target.cache->min_prefix_bytes) {
      // Below the target's prefix-byte floor: conservative drop rather than
      // sending hints computed against the primary's cache options.
      request.cache = std::nullopt;
    }
  }
}

}  // namespace

Runtime::Runtime(provider::System& backend) noexcept : backend_{&backend} {}

async::Awaitable<core::Result<provider::Response>>
Runtime::send(provider::Request request, provider::Route route, provider::EventSink* sink) const {
  if (request.retry.max_attempts == 0) {
    co_return std::unexpected(invalid_retry_policy());
  }

  auto targets = route_targets(route);
  core::Error last_retryable_error = core::Error::internal("provider execution had no attempts");
  const auto max_attempts = request.retry.max_attempts;

  for (std::size_t target_index = 0; target_index < targets.size(); ++target_index) {
    const auto& target = targets[target_index];
    const auto role = target_index == 0 ? std::string_view{"primary"} : std::string_view{"fallback"};
    for (std::uint32_t attempt = 1; attempt <= max_attempts; ++attempt) {
      auto attempt_request = request;
      if (target_index > 0) {
        apply_target_policy(attempt_request, target);
      }
      AttemptSink attempt_sink{sink};
      auto* effective_sink = sink == nullptr ? nullptr : &attempt_sink;
      auto result = co_await backend_->send(std::move(attempt_request), single_target_route(target), effective_sink);
      if (result.has_value()) {
        if (!result->model_used.has_value()) {
          result->model_used = target.model;
        }
        if (!result->route_profile_used.has_value()) {
          result->route_profile_used = target.profile;
        }
        co_return result;
      }

      auto error = with_target_context(std::move(result).error(), target, attempt, max_attempts, role);
      if (error.kind() == core::ErrorKind::cancelled || !error.retryable()) {
        co_return std::unexpected(std::move(error));
      }
      if (attempt_sink.emitted()) {
        co_return std::unexpected(std::move(error)
                                      .with("retry_skipped", "stream_already_emitted")
                                      .with("fallback_skipped", "stream_already_emitted"));
      }

      last_retryable_error = std::move(error);
      if (attempt == max_attempts) {
        break;
      }

      const auto delay = retry_delay(last_retryable_error, request.retry.initial_backoff);
      if (delay.count() > 0) {
        auto executor = co_await asio::this_coro::executor;
        auto slept = co_await async::sleep_for(executor, delay);
        if (!slept) {
          co_return std::unexpected(with_target_context(std::move(slept).error(), target, attempt, max_attempts, role));
        }
      }
    }
  }

  co_return std::unexpected(std::move(last_retryable_error));
}

}  // namespace orangutan::provider::execution

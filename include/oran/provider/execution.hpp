// include/oran/provider/execution.hpp — retry/fallback execution wrapper.
//
// `provider::execution::Runtime` is the first execution-layer owner from
// `api-portability.md`. It deliberately wraps the existing `provider::System`
// interface instead of changing adapters or the agent loop: callers can keep
// sending a `Request` to a `System`, while this decorator consumes the
// request's `RetryPolicy` and the route's fallback targets before delegating
// each concrete attempt to the wrapped backend.

#pragma once

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::provider::execution {

/// Provider execution layer for retryable errors and route fallbacks.
///
/// Semantics:
/// - `Request::retry.max_attempts` applies per route target and must be >= 1.
/// - Retryable errors (`Error::retryable()`) are retried on the same target
///   until the attempt budget is exhausted.
/// - After a retryable primary failure exhausts its attempt budget, fallback
///   targets are tried in route order with the same attempt budget.
/// - Non-retryable errors and cancellations return immediately.
/// - Retryable errors after visible stream output return immediately; retrying
///   at that point would duplicate already-rendered bytes for callers.
/// - If the backend omits `Response::model_used`, the runtime fills it with
///   the selected target model so downstream trace rows can distinguish
///   fallback successes from primary successes.
class Runtime final : public provider::System {
public:
  explicit Runtime(provider::System& backend) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::Route route, provider::EventSink* sink = nullptr) const override;

private:
  provider::System* backend_;
};

}  // namespace orangutan::provider::execution

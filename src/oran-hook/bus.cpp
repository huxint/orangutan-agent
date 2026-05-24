// src/oran-hook/bus.cpp — `hook::Bus` implementation.

#include <oran/hook/bus.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <asio/experimental/awaitable_operators.hpp>
#include <asio/this_coro.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/async/sleep.hpp>
#include <oran/core/error.hpp>
#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/hook/sink.hpp>

namespace orangutan::hook {
namespace {

using namespace std::chrono_literals;

struct SinkDecision {
  HookDecision decision;
  std::optional<std::chrono::milliseconds> elapsed{};
};

[[nodiscard]] Payload payload_for_sink(const Sink& sink, const Payload& payload) {
  auto delivered = payload;
  if (sink.kind() != SinkKind::trusted_local) {
    if (auto* after = std::get_if<ToolAfterPayload>(&delivered); after != nullptr) {
      after->data_json.reset();
    }
  }
  return delivered;
}

[[nodiscard]] HookDecision veto_from_error(const core::Error& error, std::string_view sink_id) {
  HookDecision decision{};
  decision.kind = HookDecisionKind::veto;
  // Spec 0015 v1: a blocking sink that returns or throws an error
  // surfaces as `reason = hook_error`; the underlying message rides
  // along so the (future) audit row can record the cause.
  decision.reason = "hook_error";
  if (!error.message().empty()) {
    decision.reason.append(": ");
    decision.reason.append(error.message());
  }
  decision.reason.append(" [sink=");
  decision.reason.append(sink_id);
  decision.reason.append("]");
  return decision;
}

[[nodiscard]] HookDecision veto_from_exception(std::string_view what, std::string_view sink_id) {
  HookDecision decision{};
  decision.kind = HookDecisionKind::veto;
  decision.reason = "hook_error: ";
  decision.reason.append(what);
  decision.reason.append(" [sink=");
  decision.reason.append(sink_id);
  decision.reason.append("]");
  return decision;
}

[[nodiscard]] HookDecision veto_from_timeout() {
  HookDecision decision{};
  decision.kind = HookDecisionKind::veto;
  decision.reason = "hook_timeout";
  return decision;
}

[[nodiscard]] async::Awaitable<SinkDecision> call_blocking_sink(Sink& sink, Event event, Payload payload) {
  try {
    auto result = co_await sink.handle_blocking(event, std::move(payload));
    if (!result) {
      co_return SinkDecision{.decision = veto_from_error(result.error(), sink.id())};
    }
    co_return SinkDecision{.decision = std::move(result).value()};
  } catch (const std::exception& ex) {
    co_return SinkDecision{.decision = veto_from_exception(ex.what(), sink.id())};
  } catch (...) {
    co_return SinkDecision{.decision = veto_from_exception("sink threw non-std exception", sink.id())};
  }
}

[[nodiscard]] async::Awaitable<core::Result<void>> sleep_for_timeout(std::chrono::milliseconds timeout) {
  const auto executor = co_await asio::this_coro::executor;
  co_return co_await async::sleep_for(executor, timeout);
}

[[nodiscard]] async::Awaitable<core::Result<SinkDecision>>
call_blocking_sink_with_timeout(Sink& sink, Event event, Payload payload, std::chrono::milliseconds timeout) {
  if (timeout <= 0ms) {
    co_return co_await call_blocking_sink(sink, event, std::move(payload));
  }

  using namespace asio::experimental::awaitable_operators;
  auto raced = co_await (call_blocking_sink(sink, event, std::move(payload)) || sleep_for_timeout(timeout));
  if (auto* decision = std::get_if<SinkDecision>(&raced); decision != nullptr) {
    co_return std::move(*decision);
  }

  auto timer = std::get<core::Result<void>>(std::move(raced));
  if (!timer) {
    co_return std::unexpected(std::move(timer).error());
  }

  co_return SinkDecision{
      .decision = veto_from_timeout(),
      .elapsed = timeout,
  };
}

}  // namespace

Bus::Bus(BusOptions options) : options_(options) {}

bool PublishOutcome::all_succeeded() const noexcept {
  return std::ranges::all_of(sinks, [](const auto& row) { return !row.error.has_value(); });
}

std::size_t PublishOutcome::failure_count() const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(sinks, [](const auto& row) { return row.error.has_value(); }));
}

void Bus::bind(Sink& sink, std::span<const Event> events) {
  for (const auto event : events) {
    auto& subscribers = bindings_[event];
    if (!std::ranges::contains(subscribers, &sink)) {
      subscribers.push_back(&sink);
    }
  }
}

void Bus::bind(Sink& sink, std::initializer_list<Event> events) {
  bind(sink, std::span<const Event>{events.begin(), events.size()});
}

std::size_t Bus::unbind(Sink& sink) {
  std::size_t removed = 0;
  for (auto& [_, subscribers] : bindings_) {
    const auto before = subscribers.size();
    std::erase(subscribers, &sink);
    removed += before - subscribers.size();
  }
  return removed;
}

async::Awaitable<PublishOutcome> Bus::publish_advisory(Event event, Payload payload) {
  PublishOutcome outcome;
  const auto it = bindings_.find(event);
  if (it == bindings_.end()) {
    co_return outcome;
  }
  outcome.sinks.reserve(it->second.size());
  for (auto* sink : it->second) {
    PublishOutcome::SinkResult row{.sink_id = std::string{sink->id()}, .error = std::nullopt};
    // Advisory contract: a misbehaving sink must not abort the publish for
    // subsequent sinks. A sink that throws (either directly or via its
    // awaitable) gets its exception captured as Error::internal and the
    // loop continues. Without this guard, tool dispatch — whose
    // tool_after publish is `[[maybe_unused]]` — would crash on any
    // throwing extension.
    try {
      auto result = co_await sink->receive(event, payload_for_sink(*sink, payload));
      if (!result) {
        row.error = std::move(result).error();
      }
    } catch (const std::exception& ex) {
      row.error = core::Error::internal(ex.what()).with("sink", std::string{sink->id()});
    } catch (...) {
      row.error = core::Error::internal("sink threw non-std exception").with("sink", std::string{sink->id()});
    }
    outcome.sinks.push_back(std::move(row));
  }
  co_return outcome;
}

async::Awaitable<core::Result<HookDecision>> Bus::publish_blocking_impl(Event event, Payload payload) {
  const auto it = bindings_.find(event);
  if (it == bindings_.end()) {
    co_return HookDecision{};
  }
  std::vector<HookDecisionTrace> trace;
  trace.reserve(it->second.size());
  for (auto* sink : it->second) {
    auto sink_result = co_await call_blocking_sink_with_timeout(*sink,
                                                                event,
                                                                payload_for_sink(*sink, payload),
                                                                options_.blocking_timeout);
    if (!sink_result) {
      co_return std::unexpected(std::move(sink_result).error());
    }
    auto decision = std::move(sink_result->decision);
    trace.push_back(HookDecisionTrace{
        .sink_id = std::string{sink->id()},
        .kind = decision.kind,
        .reason = decision.reason,
        .elapsed = sink_result->elapsed,
    });
    if (decision.kind != HookDecisionKind::proceed) {
      decision.trace = std::move(trace);
      co_return decision;
    }
  }
  HookDecision decision{};
  decision.trace = std::move(trace);
  co_return decision;
}

std::size_t Bus::binding_count() const noexcept {
  std::size_t total = 0;
  for (const auto& [_, subscribers] : bindings_) {
    total += subscribers.size();
  }
  return total;
}

std::size_t Bus::sink_count(Event event) const noexcept {
  const auto it = bindings_.find(event);
  return it == bindings_.end() ? std::size_t{0} : it->second.size();
}

}  // namespace orangutan::hook

// src/oran-hook/bus.cpp — `hook::Bus` implementation.

#include <oran/hook/bus.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/this_coro.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/async/channel.hpp>
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

struct AdvisorySinkResult {
  std::size_t index{};
  PublishOutcome::SinkResult row;
};

struct AdvisoryFanoutState {
  AdvisoryFanoutState(asio::any_io_executor executor, std::size_t sink_count)
      : completion(std::move(executor), sink_count), child_cancels(sink_count), total_sinks(sink_count) {}

  async::Channel<AdvisorySinkResult> completion;
  std::vector<asio::cancellation_signal> child_cancels;
  std::size_t total_sinks{};
};

[[nodiscard]] Payload payload_for_sink(const Sink& sink, const Payload& payload) {
  auto delivered = payload;
  if (sink.kind() != SinkKind::trusted_local) {
    std::visit(
        [](auto& alt) {
          if constexpr (requires {
                          alt.input_json;
                          alt.redacted_input_json;
                        }) {
            if (alt.redacted_input_json.has_value()) {
              alt.input_json = *alt.redacted_input_json;
            }
          }
          if constexpr (std::same_as<std::decay_t<decltype(alt)>, ToolAfterPayload>) {
            alt.data_json.reset();
          }
        },
        delivered);
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

[[nodiscard]] async::Awaitable<void> run_advisory_sink(std::shared_ptr<AdvisoryFanoutState> state,
                                                       Sink* sink,
                                                       Event event,
                                                       Payload payload,
                                                       std::size_t index) {
  PublishOutcome::SinkResult row{.sink_id = std::string{sink->id()}, .error = std::nullopt};
  // Advisory contract: a misbehaving sink must not abort the publish for
  // other sinks. Each fan-out child catches errors locally and reports one
  // ordered row back to the parent.
  try {
    auto result = co_await sink->receive(event, std::move(payload));
    if (!result) {
      row.error = std::move(result).error();
    }
  } catch (const std::exception& ex) {
    row.error = core::Error::internal(ex.what()).with("sink", std::string{sink->id()});
  } catch (...) {
    row.error = core::Error::internal("sink threw non-std exception").with("sink", std::string{sink->id()});
  }

  // Parent cancellation is propagated through `child_cancels`; once this child
  // has a row to report, its completion send must not be swallowed by that
  // same cancellation slot.
  co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
  [[maybe_unused]] auto sent =
      co_await state->completion.send(AdvisorySinkResult{.index = index, .row = std::move(row)});
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

  const auto& subscribers = it->second;
  auto rows = std::vector<std::optional<PublishOutcome::SinkResult>>(subscribers.size());
  auto executor = co_await asio::this_coro::executor;
  auto state = std::make_shared<AdvisoryFanoutState>(executor, subscribers.size());

  for (std::size_t i = 0; i < subscribers.size(); ++i) {
    auto* sink = subscribers[i];
    asio::co_spawn(executor,
                   run_advisory_sink(state, sink, event, payload_for_sink(*sink, payload), i),
                   asio::bind_cancellation_slot(state->child_cancels[i].slot(), asio::detached));
  }

  if (auto cancel = co_await asio::this_coro::cancellation_state; cancel.cancelled() != asio::cancellation_type::none) {
    for (auto& signal : state->child_cancels) {
      signal.emit(asio::cancellation_type::all);
    }
  }

  std::size_t completed = 0;
  while (completed < state->total_sinks) {
    auto next = co_await state->completion.receive();
    if (!next) {
      for (auto& signal : state->child_cancels) {
        signal.emit(asio::cancellation_type::all);
      }
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
      continue;
    }
    rows[next->index] = std::move(next->row);
    ++completed;
  }

  outcome.sinks.reserve(rows.size());
  for (auto& row : rows) {
    if (row.has_value()) {
      outcome.sinks.push_back(std::move(*row));
    }
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

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

#include <asio/experimental/awaitable_operators.hpp>
#include <asio/this_coro.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/async/sleep.hpp>
#include <oran/async/task_group.hpp>
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

[[nodiscard]] Payload redact_payload(Payload payload) {
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
        if constexpr (std::same_as<std::decay_t<decltype(alt)>, MemoryWritePayload>) {
          if (alt.redacted_record.has_value()) {
            alt.record.title.clear();
            alt.record.body.clear();
            alt.record.tags.clear();
            alt.record.linked_record_ids.clear();
          }
        }
        if constexpr (std::same_as<std::decay_t<decltype(alt)>, MemoryReadPayload>) {
          if (alt.redacted_query_bytes.has_value()) {
            alt.query.clear();
          }
          for (auto& hit : alt.hits) {
            if (hit.redacted_record.has_value()) {
              hit.record.title.clear();
              hit.record.body.clear();
              hit.record.tags.clear();
              hit.record.linked_record_ids.clear();
            }
          }
        }
      },
      payload);
  return payload;
}

struct SharedPayloads {
  PayloadPtr trusted;
  PayloadPtr default_;
};

[[nodiscard]] SharedPayloads make_shared_payloads(std::span<Sink* const> sinks, Payload payload) {
  const bool needs_trusted =
      std::ranges::any_of(sinks, [](const Sink* sink) { return sink->kind() == SinkKind::trusted_local; });
  const bool needs_default =
      std::ranges::any_of(sinks, [](const Sink* sink) { return sink->kind() != SinkKind::trusted_local; });

  SharedPayloads shared;
  if (needs_trusted) {
    shared.trusted = std::make_shared<Payload>(std::move(payload));
    if (needs_default) {
      shared.default_ = std::make_shared<Payload>(redact_payload(*shared.trusted));
    }
  } else if (needs_default) {
    shared.default_ = std::make_shared<Payload>(redact_payload(std::move(payload)));
  }
  return shared;
}

[[nodiscard]] PayloadPtr payload_for_sink(const Sink& sink, const SharedPayloads& payloads) noexcept {
  if (sink.kind() == SinkKind::trusted_local) {
    return payloads.trusted;
  }
  return payloads.default_;
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

[[nodiscard]] async::Awaitable<SinkDecision> call_blocking_sink(Sink& sink, Event event, PayloadPtr payload) {
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

[[nodiscard]] async::Awaitable<core::Result<void>>
run_advisory_sink(std::vector<std::optional<PublishOutcome::SinkResult>>& rows,
                  Sink& sink,
                  Event event,
                  PayloadPtr payload,
                  std::size_t index) {
  PublishOutcome::SinkResult row{.sink_id = std::string{sink.id()}, .error = std::nullopt};
  // Advisory contract: a misbehaving sink must not abort the publish for
  // other sinks. Each fan-out child catches errors locally and reports one
  // ordered row back to the parent.
  try {
    auto result = co_await sink.receive(event, std::move(payload));
    if (!result) {
      row.error = std::move(result).error();
    }
  } catch (const std::exception& ex) {
    row.error = core::Error::internal(ex.what()).with("sink", std::string{sink.id()});
  } catch (...) {
    row.error = core::Error::internal("sink threw non-std exception").with("sink", std::string{sink.id()});
  }

  rows[index] = std::move(row);
  co_return core::Result<void>{};
}

[[nodiscard]] async::Awaitable<core::Result<void>> sleep_for_timeout(std::chrono::milliseconds timeout) {
  const auto executor = co_await asio::this_coro::executor;
  co_return co_await async::sleep_for(executor, timeout);
}

[[nodiscard]] async::Awaitable<core::Result<SinkDecision>>
call_blocking_sink_with_timeout(Sink& sink, Event event, PayloadPtr payload, std::chrono::milliseconds timeout) {
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
  if (subscribers.empty()) {
    co_return outcome;
  }

  auto rows = std::vector<std::optional<PublishOutcome::SinkResult>>(subscribers.size());
  auto executor = co_await asio::this_coro::executor;
  auto tasks_result = async::TaskGroup::create(
      executor,
      async::TaskGroupOptions{.max_tasks = subscribers.size(), .max_completed = subscribers.size()});
  if (!tasks_result) {
    outcome.sinks.reserve(subscribers.size());
    for (auto* sink : subscribers) {
      outcome.sinks.push_back(
          PublishOutcome::SinkResult{.sink_id = std::string{sink->id()}, .error = tasks_result.error()});
    }
    co_return outcome;
  }
  auto tasks = std::move(*tasks_result);
  auto task_indices = std::vector<std::size_t>{};
  task_indices.reserve(subscribers.size());
  const auto payloads = make_shared_payloads(std::span<Sink* const>{subscribers}, std::move(payload));

  for (std::size_t i = 0; i < subscribers.size(); ++i) {
    auto* sink = subscribers[i];
    auto spawned = tasks.spawn("hook-advisory-" + std::to_string(i),
                               [&rows, sink, event, payload = payload_for_sink(*sink, payloads), i]() mutable {
                                 return run_advisory_sink(rows, *sink, event, std::move(payload), i);
                               });
    if (!spawned) {
      rows[i] = PublishOutcome::SinkResult{.sink_id = std::string{sink->id()}, .error = std::move(spawned).error()};
    } else {
      task_indices.push_back(i);
    }
  }

  auto report = co_await tasks.join();
  if (!report) {
    for (std::size_t i = 0; i < subscribers.size(); ++i) {
      if (!rows[i].has_value()) {
        rows[i] = PublishOutcome::SinkResult{.sink_id = std::string{subscribers[i]->id()}, .error = report.error()};
      }
    }
  } else {
    for (std::size_t task = 0; task < report->tasks.size(); ++task) {
      const auto i = task_indices[task];
      if (!rows[i].has_value() && report->tasks[task].error.has_value()) {
        rows[i] = PublishOutcome::SinkResult{.sink_id = std::string{subscribers[i]->id()},
                                             .error = std::move(report->tasks[task].error)};
      }
    }
  }

  outcome.sinks.reserve(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (!rows[i].has_value()) {
      rows[i] =
          PublishOutcome::SinkResult{.sink_id = std::string{subscribers[i]->id()},
                                     .error = core::Error::internal("advisory hook task completed without an outcome")};
    }
    outcome.sinks.push_back(std::move(*rows[i]));
  }
  co_return outcome;
}

async::Awaitable<core::Result<HookDecision>> Bus::publish_blocking_impl(Event event, Payload payload) {
  const auto it = bindings_.find(event);
  if (it == bindings_.end()) {
    co_return HookDecision{};
  }
  const auto payloads = make_shared_payloads(std::span<Sink* const>{it->second}, std::move(payload));
  std::vector<HookDecisionTrace> trace;
  trace.reserve(it->second.size());
  for (auto* sink : it->second) {
    auto sink_result = co_await call_blocking_sink_with_timeout(*sink,
                                                                event,
                                                                payload_for_sink(*sink, payloads),
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

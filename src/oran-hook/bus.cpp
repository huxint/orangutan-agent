// src/oran-hook/bus.cpp — `hook::Bus` implementation.

#include <oran/hook/bus.hpp>

#include <algorithm>
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

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/hook/sink.hpp>

namespace orangutan::hook {
namespace {

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

}  // namespace

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
  for (auto* sink : it->second) {
    HookDecision decision{};
    try {
      auto result = co_await sink->handle_blocking(event, payload_for_sink(*sink, payload));
      if (!result) {
        // Spec 0015 v1: sink error -> veto with reason=hook_error;
        // short-circuit the walk so the dispatch consumer sees a single
        // authoritative decision per call.
        co_return veto_from_error(result.error(), sink->id());
      }
      decision = std::move(result).value();
    } catch (const std::exception& ex) {
      co_return veto_from_exception(ex.what(), sink->id());
    } catch (...) {
      co_return veto_from_exception("sink threw non-std exception", sink->id());
    }
    if (decision.kind != HookDecisionKind::proceed) {
      co_return decision;
    }
  }
  co_return HookDecision{};
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

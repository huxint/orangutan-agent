// src/oran-hook/bus.cpp — `hook::Bus` implementation.

#include <oran/hook/bus.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/hook/sink.hpp>

namespace orangutan::hook {

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
    auto result = co_await sink->receive(event, payload);
    PublishOutcome::SinkResult row{.sink_id = std::string{sink->id()}, .error = std::nullopt};
    if (!result) {
      row.error = std::move(result).error();
    }
    outcome.sinks.push_back(std::move(row));
  }
  co_return outcome;
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

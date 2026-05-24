// include/oran/hook/bus.hpp — in-process hook bus.
//
// The bus is the single point of fan-out for runtime lifecycle events. The
// agent loop, the tool registry, and (eventually) the provider system and
// memory tier all publish through one bus instance; sinks subscribe to the
// events they care about. The bootstrap layer constructs the bus and binds
// the configured sinks once per process.
//
// Slice 22 ships *advisory* publish only: every sink subscribed to an event
// receives the (event, payload) pair, each sink's success/failure is
// captured in the returned `PublishOutcome`, but no sink can veto the
// publish or the action that triggered it. Blocking semantics with veto
// land in a follow-up slice when the first blocking consumer (likely
// `permission_ask_rendered`) needs them.
//
// Concurrency. The bus is not thread-safe; the runtime owns one per strand.
// Subscribers (`Sink&`) are non-owning — the caller keeps them alive for the
// bus's lifetime.

#pragma once

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/event_traits.hpp>
#include <oran/hook/payload.hpp>
#include <oran/hook/sink.hpp>

namespace orangutan::hook {

/// Result of one `publish_advisory` call. Lists every sink that received
/// the event in subscription order, with the per-sink error (if any). The
/// caller can iterate this to surface sink failures into logs or audit.
struct PublishOutcome {
  struct SinkResult {
    std::string sink_id;
    std::optional<core::Error> error;
  };
  std::vector<SinkResult> sinks;

  /// True when every sink returned success. An empty `sinks` (no sink
  /// subscribed) is also `true` — the publish was a no-op success.
  [[nodiscard]] bool all_succeeded() const noexcept;

  /// Number of sinks that reported an error.
  [[nodiscard]] std::size_t failure_count() const noexcept;
};

class Bus {
public:
  Bus() = default;
  ~Bus() = default;

  Bus(const Bus&) = delete;
  Bus& operator=(const Bus&) = delete;
  Bus(Bus&&) noexcept = default;
  Bus& operator=(Bus&&) noexcept = default;

  /// Subscribe `sink` to every event in `events`. Same sink can be bound
  /// to different events with separate calls; duplicate (sink, event)
  /// pairs are deduplicated so re-binding the same pair is a no-op.
  void bind(Sink& sink, std::span<const Event> events);

  /// Convenience overload for brace-enclosed event lists.
  void bind(Sink& sink, std::initializer_list<Event> events);

  /// Remove every subscription `sink` holds. Returns the number of
  /// bindings that were removed; zero when the sink was not subscribed.
  std::size_t unbind(Sink& sink);

  /// Publish `event` + `payload` to every sink subscribed to `event`,
  /// awaiting each sink in subscription order. Sink failures are
  /// captured in the returned outcome but do not abort the publish for
  /// subsequent sinks (advisory semantics — see file header).
  [[nodiscard]] async::Awaitable<PublishOutcome> publish_advisory(Event event, Payload payload);

  /// Publish `event` + `payload` as a blocking call (spec 0015 v1).
  /// Walks subscribed sinks in subscription order, calling each one's
  /// `Sink::handle_blocking`. The first non-`proceed` decision short-
  /// circuits the walk and is returned. With no sinks subscribed, or
  /// when every sink returns `proceed`, the bus returns a default-
  /// constructed `HookDecision{}` (kind = `proceed`).
  ///
  /// A sink that returns `std::unexpected(error)` or throws is treated
  /// as `veto` with `reason = "hook_error"`; the underlying error is
  /// converted into the same `HookDecision::reason` so the audit layer
  /// can record the cause once dispatch consumption lands.
  ///
  /// The whitelist of blocking events is encoded in `EventTraits<E>`;
  /// calling `publish_blocking<Event::tool_after>` fails to compile.
  template <Event E>
    requires HasBlockingDecision<E>
  [[nodiscard]] async::Awaitable<core::Result<HookDecision>> publish_blocking(Payload payload) {
    return publish_blocking_impl(E, std::move(payload));
  }

  /// Total `(sink, event)` bindings across all events.
  [[nodiscard]] std::size_t binding_count() const noexcept;

  /// Number of sinks subscribed to `event`.
  [[nodiscard]] std::size_t sink_count(Event event) const noexcept;

private:
  /// Runtime body for `publish_blocking<E>`. Lives in `bus.cpp` so the
  /// template instantiation is a thin forward and the per-TU compile
  /// cost stays bounded.
  [[nodiscard]] async::Awaitable<core::Result<HookDecision>> publish_blocking_impl(Event event, Payload payload);

  std::unordered_map<Event, std::vector<Sink*>> bindings_;
};

}  // namespace orangutan::hook

#pragma once

#include <chrono>
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

/// Runtime policy for hook dispatch. The default blocking timeout mirrors
/// `config.hooks.timeout_ms` in the operator config surface.
struct BusOptions {
  std::chrono::milliseconds blocking_timeout{2000};
  /// Deadline for the advisory fan-out join. A sink that ignores
  /// cancellation is abandoned after this bound (its outcome row records a
  /// hook error); zero disables the bound and joins unconditionally.
  std::chrono::milliseconds advisory_timeout{2000};
  /// Human approval may take longer than an extension callback. When absent,
  /// permission prompts use the ordinary blocking timeout.
  std::optional<std::chrono::milliseconds> approval_timeout{};

  friend bool operator==(const BusOptions&, const BusOptions&) = default;
};

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
  explicit Bus(BusOptions options = {});
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

  /// Publish `event` + `payload` to every sink subscribed to `event`.
  /// Sinks are started as concurrent child coroutines; the returned
  /// outcome rows remain in subscription order. Sink failures are
  /// captured in the returned outcome but do not abort the publish for
  /// sibling sinks (advisory semantics — see file header). The publish
  /// owns those children through a bounded task group and joins them
  /// before returning, including after parent cancellation, so callers
  /// may release the non-owning sink references once the await completes.
  /// With `BusOptions::advisory_timeout` set, a sink that ignores
  /// cancellation is abandoned at the deadline: its outcome row carries a
  /// hook error naming the sink, and the abandoned child may still be
  /// running — callers must not destroy such a sink until it winds down
  /// (production owners keep sinks for process lifetime).
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
  /// converted into the same `HookDecision::reason` so the dispatch/audit
  /// layer can record the cause.
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

  /// Current dispatch policy. Exposed for bootstrap/tests; mutation goes
  /// through `set_options` so future invariants stay centralized.
  [[nodiscard]] const BusOptions& options() const noexcept {
    return options_;
  }

  void set_options(BusOptions options) noexcept {
    options_ = options;
  }

private:
  /// Runtime body for `publish_blocking<E>`. Lives in `bus.cpp` so the
  /// template instantiation is a thin forward and the per-TU compile
  /// cost stays bounded.
  [[nodiscard]] async::Awaitable<core::Result<HookDecision>> publish_blocking_impl(Event event, Payload payload);

  std::unordered_map<Event, std::vector<Sink*>> bindings_;
  BusOptions options_{};
};

}  // namespace orangutan::hook

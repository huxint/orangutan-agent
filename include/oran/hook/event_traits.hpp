// include/oran/hook/event_traits.hpp — per-event blocking decision shape.
//
// Maps each `hook::Event` to the decision type a blocking sink can return.
// Only the events listed in spec 0015 v1's whitelist
// (`tool_before`, `permission_ask_rendered`, `memory_write_before`) carry a
// `Decision = HookDecision` member; every other event leaves the trait
// empty so `Bus::publish_blocking<E>` fails to compile against them.
//
// The trait surface is intentionally tiny: it is consulted only at the
// `publish_blocking<E>` call site through `hook::HasBlockingDecision<E>`.
// The runtime path uses the generic `HookDecision` value type from
// `decision.hpp` so a single `Bus::publish_blocking_impl` covers every
// event.

#pragma once

#include <concepts>

#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>

namespace orangutan::hook {

/// Primary template — left empty so events outside the v1 whitelist
/// fail to satisfy `HasBlockingDecision`. Adding a new blocking event
/// is a single specialisation below.
template <Event E>
struct EventTraits {};

template <>
struct EventTraits<Event::tool_before> {
  using Decision = HookDecision;
};

template <>
struct EventTraits<Event::permission_ask_rendered> {
  using Decision = HookDecision;
};

template <>
struct EventTraits<Event::memory_write_before> {
  using Decision = HookDecision;
};

/// True iff `E` has a blocking `Decision` of type `HookDecision`. Used
/// as the `requires` constraint on `Bus::publish_blocking<E>`.
template <Event E>
concept HasBlockingDecision =
    requires { typename EventTraits<E>::Decision; } && std::same_as<typename EventTraits<E>::Decision, HookDecision>;

}  // namespace orangutan::hook

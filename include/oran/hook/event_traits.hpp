#pragma once

#include <concepts>

#include <oran/hook/decision.hpp>
#include <oran/hook/event.hpp>

namespace orangutan::hook {

/// Effect gates specialize this trait with their blocking decision type.
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

/// Constrains `Bus::publish_blocking` to effect gates.
template <Event E>
concept HasBlockingDecision =
    requires { typename EventTraits<E>::Decision; } && std::same_as<typename EventTraits<E>::Decision, HookDecision>;

}  // namespace orangutan::hook

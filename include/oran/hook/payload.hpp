// include/oran/hook/payload.hpp — typed payloads for hook events.
//
// Slice 22 ships typed shapes for the events that the immediate consumers
// produce: the four tool-lifecycle events (`tool_before`, `tool_dispatched`,
// `tool_after`, `tool_error`) — though the `Registry::dispatch` wiring in
// slice 22 only publishes `tool_before` and `tool_after`. Events without a
// typed shape yet carry `std::monostate` so sinks subscribed to them can
// observe occurrence without payload content; typed shapes land with the
// producing subsystem.

#pragma once

#include <chrono>
#include <string>
#include <variant>

#include <oran/core/time.hpp>

namespace orangutan::hook {

/// Identity columns every event payload duplicates. Captured once on the
/// dispatch context and copied per event so sinks don't have to chase the
/// dispatch context's reference back up the stack.
struct Identity {
  std::string scope_key;
  std::string agent_key;
  std::string identity;  // operator / agent identity
};

/// Pre-dispatch payload. Published just after the registry resolves the
/// tool definition but before permission evaluation, so sinks see every
/// known call attempt regardless of how the call is gated.
struct ToolBeforePayload {
  std::string tool_name;
  std::string input_json;
  Identity who;
  /// Wall-clock instant the dispatch started — sinks correlate
  /// `tool_before` with `tool_after` via this field plus tool_name.
  core::Time started_at{};
};

/// Post-dispatch payload. Published at every exit from `Registry::dispatch`
/// (handler returned, permission denied, broker rejection, audit error).
/// The `succeeded` boolean tracks the dispatch outcome; on failure
/// `error_kind` / `error_message` carry the propagated error.
struct ToolAfterPayload {
  std::string tool_name;
  std::string input_json;
  Identity who;
  bool succeeded{false};
  /// Verbatim `Output::text` on success; empty string on failure.
  std::string output_text;
  /// `core::Error::kind` enumerator wire spelling (e.g. `permission_denied`,
  /// `not_found`, `internal`) on failure; empty on success.
  std::string error_kind;
  /// `core::Error::message` on failure; empty on success.
  std::string error_message;
  core::Time started_at{};
  core::Time finished_at{};
  /// `finished_at - started_at`. Stored separately so sinks don't have to
  /// recompute it from two `core::Time` values.
  std::chrono::nanoseconds duration{0};
};

/// `std::monostate` is the placeholder for events whose typed payload has
/// not landed yet — provider, memory, channel, orchestration, automation,
/// session, and permission ask-flow events. Sinks subscribed to those
/// events receive the variant in its monostate alternative; they can still
/// react to the occurrence and the event kind.
using Payload = std::variant<std::monostate, ToolBeforePayload, ToolAfterPayload>;

}  // namespace orangutan::hook

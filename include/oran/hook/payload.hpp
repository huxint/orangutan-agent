// include/oran/hook/payload.hpp — typed payloads for hook events.
//
// Slices 22 + 25 ship typed shapes for the four tool-lifecycle events
// (`tool_before`, `tool_dispatched`, `tool_after`, `tool_error`). Events
// without a typed shape yet carry `std::monostate` so sinks subscribed to
// them can observe occurrence without payload content; typed shapes land
// with the producing subsystem.

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

/// Pre-handler payload. Published only when the handler is about to run —
/// i.e. on the `allow` verdict OR on an `ask` verdict that the approval
/// broker promoted to `approved`. Sinks subscribed to `tool_dispatched`
/// see only the calls whose handlers will actually execute, which is the
/// natural hook point for last-mile rate-limiting or "ready to invoke"
/// logging without filtering out the deny/short-circuit/reject paths.
/// `verdict` is the `permission::Verdict` enum's wire spelling (`allow`
/// or `ask`) so sinks can distinguish a free allow from an approved ask.
struct ToolDispatchedPayload {
  std::string tool_name;
  std::string input_json;
  Identity who;
  core::Time started_at{};
  std::string verdict;
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

/// Failure-only narrow payload. Published alongside `tool_after` whenever
/// the dispatch result is an error (permission deny, broker rejection,
/// audit error, handler error). Sinks that only care about failures
/// (e.g. a Slack alerter, a failure-bucket counter) subscribe to
/// `tool_error` and skip the `tool_after::succeeded` filter dance.
struct ToolErrorPayload {
  std::string tool_name;
  std::string input_json;
  Identity who;
  /// `core::Error::kind` enumerator wire spelling
  /// (e.g. `permission_denied`, `not_found`, `internal`).
  std::string error_kind;
  std::string error_message;
  core::Time started_at{};
  core::Time finished_at{};
  std::chrono::nanoseconds duration{0};
};

/// `std::monostate` is the placeholder for events whose typed payload has
/// not landed yet — provider, memory, channel, orchestration, automation,
/// session, and permission ask-flow events. Sinks subscribed to those
/// events receive the variant in its monostate alternative; they can still
/// react to the occurrence and the event kind.
using Payload =
    std::variant<std::monostate, ToolBeforePayload, ToolDispatchedPayload, ToolAfterPayload, ToolErrorPayload>;

}  // namespace orangutan::hook

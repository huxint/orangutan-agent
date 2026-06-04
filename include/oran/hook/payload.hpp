// include/oran/hook/payload.hpp — typed payloads for hook events.
//
// Slices 22 + 25 ship typed shapes for the four tool-lifecycle events
// (`tool_before`, `tool_dispatched`, `tool_after`, `tool_error`). Slice 94
// adds the first permission ask-flow shape (`permission_ask_rendered`) so UI
// sinks can render an approval prompt and return a blocking decision. Slice
// 152 adds optional per-sink redacted input views for sensitive mutation
// payloads. Events without a typed shape yet carry `std::monostate` so sinks
// subscribed to them can observe occurrence without payload content; typed
// shapes land with the producing subsystem.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include <oran/core/time.hpp>
#include <oran/core/turn_id.hpp>

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
  /// Optional sanitized input view. When present, `Bus` delivers this value
  /// as `input_json` to sinks whose `Sink::kind()` is not
  /// `SinkKind::trusted_local`; trusted-local sinks receive the original
  /// `input_json`.
  std::optional<std::string> redacted_input_json{};
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
  /// Optional sanitized input view for non-trusted sinks. See
  /// `ToolBeforePayload::redacted_input_json`.
  std::optional<std::string> redacted_input_json{};
  Identity who;
  core::Time started_at{};
  std::string verdict;
};

/// Usage metrics copied from `tool::Output::usage` without making
/// `oran-hook` depend on `oran-tool`.
struct ToolUsage {
  std::optional<std::uintmax_t> bytes_read{};
  std::optional<std::uintmax_t> bytes_written{};
  std::optional<std::uint32_t> files_touched{};
  std::optional<std::uint64_t> match_count{};
  std::optional<double> cost_estimate{};
  std::optional<std::chrono::nanoseconds> wall_time{};
  bool truncated{false};
  bool data_dropped{false};

  friend bool operator==(const ToolUsage&, const ToolUsage&) = default;
};

/// Post-dispatch payload. Published at every exit from `Registry::dispatch`
/// (handler returned, permission denied, broker rejection, audit error).
/// The `succeeded` boolean tracks the dispatch outcome; on failure
/// `error_kind` / `error_message` carry the propagated error.
struct ToolAfterPayload {
  std::string tool_name;
  std::string input_json;
  /// Optional sanitized input view for non-trusted sinks. See
  /// `ToolBeforePayload::redacted_input_json`.
  std::optional<std::string> redacted_input_json{};
  Identity who;
  bool succeeded{false};
  /// Verbatim `Output::text` on success; empty string on failure.
  std::string output_text;
  /// Raw structured output bytes copied from `Output::data_json` on success.
  /// `Bus` redacts this field for sinks whose `Sink::kind()` is not
  /// `SinkKind::trusted_local`.
  std::optional<std::string> data_json{};
  /// Metrics copied from `Output::usage` on success; all fields empty on
  /// dispatch failure.
  ToolUsage usage{};
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
  /// Optional sanitized input view for non-trusted sinks. See
  /// `ToolBeforePayload::redacted_input_json`.
  std::optional<std::string> redacted_input_json{};
  Identity who;
  /// `core::Error::kind` enumerator wire spelling
  /// (e.g. `permission_denied`, `not_found`, `internal`).
  std::string error_kind;
  std::string error_message;
  core::Time started_at{};
  core::Time finished_at{};
  std::chrono::nanoseconds duration{0};
};

/// Approval prompt payload. Published when a permission rule returns `ask`
/// and the dispatch has an approval broker but no caller-supplied token.
/// UI-facing sinks render this into their own channel-specific prompt and
/// return a blocking decision: `proceed` approves, `veto` denies.
struct PermissionAskRenderedPayload {
  std::string tool_name;
  std::string input_json;
  /// Optional sanitized input view for non-trusted sinks. See
  /// `ToolBeforePayload::redacted_input_json`.
  std::optional<std::string> redacted_input_json{};
  Identity who;
  /// Human-readable rule/hook reason that caused the ask.
  std::string decision_reason;
  /// Replay policy copied from the matched permission decision.
  std::uint32_t replay_max{8};
  std::chrono::seconds approval_ttl{3600};
  core::Time requested_at{};
};

/// Provider token/cost counters copied without making `oran-hook` depend on
/// `oran-provider`.
struct ProviderUsage {
  std::uint64_t input_tokens{0};
  std::uint64_t output_tokens{0};
  std::uint64_t cache_creation_tokens{0};
  std::uint64_t cache_read_tokens{0};
  std::optional<double> cost_estimate{};

  friend bool operator==(const ProviderUsage&, const ProviderUsage&) = default;
};

/// Provider request metadata. The payload intentionally carries counts and
/// route identity, not prompt text, message bodies, headers, or credentials.
struct ProviderRequestPayload {
  Identity who;
  std::string origin;
  std::optional<core::TurnId> turn_id{};
  std::uint32_t iteration{0};
  std::string route_profile;
  std::string route_model;
  std::string route_protocol;
  std::size_t fallback_count{0};
  std::size_t message_count{0};
  std::size_t tool_count{0};
  bool stream{true};
  std::optional<std::uint32_t> max_tokens{};
  std::optional<std::uint32_t> thinking_budget{};
  std::uint32_t retry_max_attempts{0};
  std::chrono::milliseconds retry_initial_backoff{0};
  core::Time started_at{};
};

/// Provider response metadata. `served_*` names the concrete route target that
/// produced the response after execution-layer retry/fallback attribution.
struct ProviderResponsePayload {
  Identity who;
  std::string origin;
  std::optional<core::TurnId> turn_id{};
  std::uint32_t iteration{0};
  std::string route_profile;
  std::string route_model;
  std::string route_protocol;
  std::string served_profile;
  std::string served_model;
  std::string served_protocol;
  std::string stop_reason;
  ProviderUsage usage{};
  core::Time started_at{};
  core::Time finished_at{};
  std::chrono::nanoseconds duration{0};
};

/// Provider error metadata. Advisory sinks see the failure class and message,
/// while raw request/response bodies stay outside the hook payload.
struct ProviderErrorPayload {
  Identity who;
  std::string origin;
  std::optional<core::TurnId> turn_id{};
  std::uint32_t iteration{0};
  std::string route_profile;
  std::string route_model;
  std::string route_protocol;
  std::string error_kind;
  std::string error_message;
  bool retryable{false};
  core::Time started_at{};
  core::Time finished_at{};
  std::chrono::nanoseconds duration{0};
};

/// Provider fallback metadata. Published when the served route profile differs
/// from the primary route profile for the turn.
struct ProviderFallbackPayload {
  Identity who;
  std::string origin;
  std::optional<core::TurnId> turn_id{};
  std::uint32_t iteration{0};
  std::string primary_profile;
  std::string primary_model;
  std::string primary_protocol;
  std::string served_profile;
  std::string served_model;
  std::string served_protocol;
  core::Time started_at{};
  core::Time finished_at{};
  std::chrono::nanoseconds duration{0};
};

/// `std::monostate` is the placeholder for events whose typed payload has
/// not landed yet — memory, channel, orchestration, automation, and session
/// events. Sinks subscribed to those events receive the variant in its
/// monostate alternative; they can still react to the occurrence and the
/// event kind.
using Payload = std::variant<std::monostate,
                             ToolBeforePayload,
                             ToolDispatchedPayload,
                             ToolAfterPayload,
                             ToolErrorPayload,
                             PermissionAskRenderedPayload,
                             ProviderRequestPayload,
                             ProviderResponsePayload,
                             ProviderErrorPayload,
                             ProviderFallbackPayload>;

/// Shared immutable payload delivered to sinks. `Bus` builds at most one raw
/// payload and one redacted payload per publish, then shares those snapshots
/// across subscribed sinks so multi-sink fan-out does not clone the same
/// structured payload for every receiver.
using PayloadPtr = std::shared_ptr<const Payload>;

}  // namespace orangutan::hook

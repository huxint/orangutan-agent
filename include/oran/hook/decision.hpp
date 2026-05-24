// include/oran/hook/decision.hpp — blocking-hook decision value types.
//
// Slice 90 opens the spec-0015 v1 surface. `HookDecision` is what a blocking
// sink returns from `Sink::handle_blocking`; the bus collects the first
// non-`proceed` decision from the subscribed sinks and forwards it to the
// dispatch pipeline (consumer wiring lands in a follow-up slice — see
// `docs/product-specs/0015-blocking-hook-decisions.md` v1 acceptance
// criteria 1-4).
//
// The rewritten input is serialized JSON bytes so the public header stays
// `nlohmann`-free per `docs/rules/critical-rules.md` C6; consumers that
// need a structured form parse the bytes inside their own TU.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <oran/core/time.hpp>

namespace orangutan::hook {

/// Decision a blocking sink returns. See spec 0015 for semantics.
enum class HookDecisionKind : std::uint8_t {
  /// Default. Allow the dispatch pipeline to continue with the original
  /// input. Sinks that observe-only should leave their decision in this
  /// state.
  proceed,
  /// Treat the dispatch as `permission_denied`. The handler is not run.
  /// The sink's `reason` is recorded on the audit row.
  veto,
  /// Substitute `rewritten_input_json` for the original input before
  /// permission evaluation runs. The handler receives the rewritten
  /// input. The audit row records both `input_hash` and
  /// `rewritten_input_hash`.
  rewrite,
  /// Route the dispatch through the approval broker even when the
  /// permission engine would otherwise have returned `allow`. The
  /// optional `approval_expires_at` overrides the rule-level TTL when
  /// set.
  require_approval,
};

/// Returned by every blocking publish. The `kind` carries the
/// short-circuit semantics; `reason` is free-form text that travels to
/// the audit row; `rewritten_input_json` is required when `kind ==
/// rewrite` and ignored otherwise; `approval_expires_at` is optional
/// metadata for `require_approval`.
///
/// The bus initialises every decision with `kind = proceed` and an empty
/// `reason`. A sink that does nothing (or has no blocking handler bound)
/// therefore yields a proceed decision and the next subscribed sink runs.
struct HookDecision {
  HookDecisionKind kind{HookDecisionKind::proceed};
  std::string reason;
  /// Serialised JSON bytes — same envelope as
  /// `ToolBeforePayload::input_json`. Stored as a string so the public
  /// header does not need to drag in `<nlohmann/json.hpp>`.
  std::optional<std::string> rewritten_input_json;
  /// Optional override for the broker TTL when `kind == require_approval`.
  /// Ignored for every other kind.
  std::optional<core::Time> approval_expires_at;
};

}  // namespace orangutan::hook

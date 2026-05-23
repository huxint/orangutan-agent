// include/oran/permission/audit.hpp — permission decision audit pipeline.
//
// Closes the in-process half of `docs/product-specs/0008-permissions.md`
// criterion 1 ("a tool call whose input matches a `deny` rule returns
// `Error::permission_denied` and is recorded in audit"). The full
// criterion lands when the agent loop wires this sink end-to-end; the
// permission library on its own ships the value types and the sink
// abstraction so any future runtime caller can record decisions
// without coupling to a specific storage backend.
//
// Why an abstract sink rather than a hard-coded SQLite write. Three
// foreseeable backends share the same vocabulary: the SQLite
// `storage::AuditRepository` for ordinary runtimes, a fire-and-forget
// `WebhookSink` for runtimes that ship events to an external SIEM,
// and the in-memory `RecordingAuditSink` for tests and operator
// inspection. Keeping the interface abstract means the agent loop
// always calls `co_await sink.record(event)` and never has to branch
// on the backend.
//
// Why the interface is async. The default storage backend writes via
// `storage::AuditRepository::append_event`, which is itself
// async-returning. Forcing a sync interface would either block the
// agent loop on disk IO (bad) or hide errors behind fire-and-forget
// (worse). Awaiting in line with the rest of the agent loop keeps the
// error model uniform.
//
// Concurrency. Sinks are not thread-safe. The agent loop owns a
// single sink per strand; an `asio::strand` wrapping a sink is the
// right pattern if a future use case calls `record` concurrently.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::permission {

/// Outcome of a permission decision as it gets recorded in the audit
/// log. The first three values mirror `Verdict` 1:1; `approved` and
/// `rejected` capture the post-broker state for an `ask` decision.
///
/// Wire spelling and parse use the generic `core::enum_name` /
/// `core::parse_enum<AuditOutcome>` helpers — no hand-maintained
/// string table per `docs/rules/code-style.md` "Enums".
enum class AuditOutcome : std::uint8_t {
  allow,
  deny,
  ask,
  approved,
  rejected,
};

/// One row destined for the audit log. The value type matches the
/// `storage::AppendAuditEventRequest` columns 1:1 so the storage
/// adapter can build the request without translation.
struct AuditEvent {
  /// Optional caller-supplied timestamp. Storage adapters that stamp
  /// `created_at` from the database ignore this field; the in-memory
  /// `RecordingAuditSink` keeps it verbatim so tests can pin the
  /// recorded time deterministically.
  core::Time recorded_at{};
  /// Per-process scope per `secrets-and-state.md` "Identity And Scope".
  /// Required by the storage adapter.
  std::string scope_key;
  /// Agent making the call.
  std::string agent_key;
  /// Tool the call targeted.
  std::string tool_name;
  /// Operator/agent identity bound to the call (matches
  /// `ApprovalAuthority::verify`'s `identity` parameter).
  std::string identity;
  /// Raw verdict from `RuleSet::evaluate`. Preserved separately from
  /// `outcome` so a forensic query can tell whether an `approved`
  /// row came from an `ask` rule or from a no-rule mode-default.
  Verdict verdict{Verdict::deny};
  /// Rendered outcome — `verdict` plus whatever happened during
  /// approval (for `ask` decisions). Allows the audit log to record
  /// the full lifecycle without joining tables.
  AuditOutcome outcome{AuditOutcome::deny};
  /// Decision reason from `Decision::reason` or broker error
  /// `Error::context()["reason"]`.
  std::string reason;
  /// SHA-256 of the call input. `nullopt` when the callsite did not
  /// compute it (raw allow/deny path). The storage adapter encodes
  /// it as 64-char lowercase hex.
  std::optional<std::array<std::byte, 32>> input_hash{};
  /// Optional parent agent-turn id. When the loop is running with trace
  /// correlation enabled, every tool decision row carries this id so it can
  /// join to `storage::trace_turns.turn_id`.
  std::optional<core::TurnId> parent_turn_id{};
  /// Free-form structured metadata. Defaults to `"{}"`.
  std::string metadata_json{"{}"};
};

/// In-place metadata replacement for a previously recorded audit row. The
/// original decision row stays durable before any handler effects; callers use
/// this to add post-result metadata such as structured tool usage without
/// appending a second permission-decision row.
struct AuditMetadataUpdate {
  std::string scope_key;
  std::string agent_key;
  std::string tool_name;
  std::string identity;
  std::optional<std::array<std::byte, 32>> input_hash{};
  std::optional<core::TurnId> parent_turn_id{};
  std::string previous_metadata_json{"{}"};
  std::string metadata_json{"{}"};
};

/// Abstract audit sink. Implementations: `NullAuditSink` (no-op),
/// `RecordingAuditSink` (in-memory capture, for tests), and (in a
/// separate header) `StorageAuditSink` (writes to
/// `storage::AuditRepository`).
class AuditSink {
public:
  AuditSink() = default;
  virtual ~AuditSink() = default;

  AuditSink(const AuditSink&) = delete;
  AuditSink& operator=(const AuditSink&) = delete;
  AuditSink(AuditSink&&) = delete;
  AuditSink& operator=(AuditSink&&) = delete;

  /// Record `event` to the underlying sink. The contract is "by the
  /// time the awaitable resolves, the event is durable enough for
  /// the implementer's semantics" — for the storage backend that
  /// means the SQLite WAL commit completed; for the recording sink
  /// it means the in-memory vector has been appended.
  [[nodiscard]] virtual async::Awaitable<core::Result<void>> record(AuditEvent event) = 0;

  /// Replace a previously recorded row's metadata. The default is a no-op so
  /// sinks that only care about permission decisions can ignore enrichment.
  [[nodiscard]] virtual async::Awaitable<core::Result<void>> update_metadata(AuditMetadataUpdate update);
};

/// No-op sink. Returns success immediately and discards every event.
/// The default for runtimes that disable auditing.
class NullAuditSink final : public AuditSink {
public:
  [[nodiscard]] async::Awaitable<core::Result<void>> record(AuditEvent event) override;
};

/// In-memory sink. Captures every recorded event in insertion order.
/// Used by tests and by runtime modes where audit lives in memory only
/// (`audit.db` disabled).
class RecordingAuditSink final : public AuditSink {
public:
  [[nodiscard]] async::Awaitable<core::Result<void>> record(AuditEvent event) override;
  [[nodiscard]] async::Awaitable<core::Result<void>> update_metadata(AuditMetadataUpdate update) override;

  [[nodiscard]] std::span<const AuditEvent> events() const noexcept {
    return events_;
  }
  void clear() noexcept {
    events_.clear();
  }

private:
  std::vector<AuditEvent> events_;
};

/// Translate a `Verdict` into the matching `AuditOutcome` (used by the
/// rule-engine emit path). `Verdict::allow` -> `AuditOutcome::allow`;
/// `Verdict::deny` -> `AuditOutcome::deny`; `Verdict::ask` ->
/// `AuditOutcome::ask`. Approval-flow callsites overwrite `outcome` to
/// `approved` or `rejected` after the broker finishes.
[[nodiscard]] constexpr AuditOutcome verdict_to_outcome(Verdict verdict) noexcept {
  switch (verdict) {
    case Verdict::allow:
      return AuditOutcome::allow;
    case Verdict::deny:
      return AuditOutcome::deny;
    case Verdict::ask:
      return AuditOutcome::ask;
  }
  return AuditOutcome::deny;
}

/// Build a partial `AuditEvent` from a `Decision`. Fills `verdict`,
/// `outcome`, and `reason`; the caller fills tool/identity/scope/
/// input_hash/metadata. Useful so callsites can write the boilerplate
/// in one line instead of mirroring `Decision`'s fields manually.
[[nodiscard]] AuditEvent make_audit_event_from_decision(const Decision& decision);

/// Encode a 32-byte SHA-256 digest as 64-char lowercase hex — the
/// wire spelling the storage adapter ingests. Exposed so callsites
/// (and tests) can produce the same encoding without rolling their
/// own hex encoder.
[[nodiscard]] std::string to_hex(std::span<const std::byte, 32> input_hash);

}  // namespace orangutan::permission

template <>
struct std::formatter<orangutan::permission::AuditOutcome> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::permission::AuditOutcome o, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(o), ctx);
  }
};

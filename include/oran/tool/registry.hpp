// include/oran/tool/registry.hpp — tool registry surface.
//
// Slice 17 introduced the registry. The current shape composes five things
// the upstream layers (`oran-permission`, `oran-async`, `oran-io`,
// `oran-hook`) already shipped:
//
//   1. A `core::ToolDef`-keyed catalog the agent loop (future slice) can
//      advertise to a provider.
//   2. A `dispatch` flow that walks one call through
//      `permission::RuleSet::evaluate` and the `permission::AuditSink`
//      handed in via `DispatchContext`, so every tool invocation produces
//      exactly one audit row matching its permission decision.
//   3. A capability-aware glue between `core::ToolDef::required_capabilities`
//      and `Rule::capability`, so a `Capability::read_file`-scoped rule
//      only fires for tools that declared the capability.
//   4. The slice-21 approval-broker bridge: when a rule fires
//      `Verdict::ask` and the caller supplies a
//      `permission::ApprovalBroker` plus a `permission::ApprovalToken`
//      in the context, dispatch consults the broker, promotes the
//      audit outcome to `approved`/`rejected`, and either runs the
//      handler or forwards the broker's rejection verbatim. When no
//      broker or no token is supplied, the short-circuit
//      `approval_required` path is preserved — but the resulting
//      `Error` now carries `replay_max` / `approval_ttl_seconds` /
//      `decision_reason` context entries so the agent loop can hand
//      them straight to `ApprovalBroker::approve` without re-evaluating
//      the rule set.
//   5. The slice-22 hook-bus tap: when the caller supplies a
//      `hook::Bus*` on the context, dispatch publishes
//      `hook::Event::tool_before` after the registry resolves the
//      tool def and `hook::Event::tool_after` at every exit (handler
//      success, permission denied, broker rejection, audit error).
//      Hooks are advisory in this slice — sinks observe but cannot
//      veto. Sinks subscribed to other events (provider, memory, …)
//      are unaffected.
//
// What this slice does NOT cover. Blocking hook semantics with veto,
// `tool_dispatched` / `tool_error` events, output scrubbing through
// `oran-log::redact`, and deferred-tool promotion all live in later
// slices.
//
// Why a single `DispatchContext` rather than the design-doc's parameter
// fan-out. Passing `permission::RuleSet&`, `permission::AuditSink&`,
// `permission::Mode`, scope/agent/identity, the executor, an optional
// broker + token, a wall-clock, and now an optional `hook::Bus*` as
// separate arguments to `dispatch` and to every handler quickly turns the
// `Handler` signature into a maintenance hazard. A typed struct makes it
// cheap to add the rest of the hook lifecycle later — handlers keep the
// same signature; new context fields are additive.
//
// Concurrency. Like `permission::AuditSink`, the registry is not
// thread-safe. The agent loop owns one registry per strand; a future
// concurrent caller wraps the registry in an `asio::strand` rather than
// reaching for an internal mutex (the legacy code's per-member mutex
// pattern was a recurring source of contention — see
// `docs/references/orangutan-legacy-audit.md`).

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/hook/bus.hpp>
#include <oran/permission/approval.hpp>
#include <oran/permission/approval_broker.hpp>
#include <oran/permission/audit.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::tool {

/// One tool's response. The structured JSON / attachment / cost surface from
/// the design doc lands when a tool genuinely needs them; the first built-in
/// (`file.read`) is text-only so this slice keeps the shape minimal.
struct Output {
  std::string text;
  bool is_error{false};

  friend bool operator==(const Output&, const Output&) = default;
};

/// Per-call context the registry threads into every handler. Holds references
/// to the long-lived permission infrastructure plus the per-call identity
/// strings the audit pipeline needs.
///
/// The struct is non-copyable / non-movable (it holds references); the caller
/// brace-initialises one on the stack per `dispatch` invocation.
struct DispatchContext {
  asio::any_io_executor executor;
  permission::Mode mode{permission::Mode::default_};
  permission::RuleSet& rules;
  permission::AuditSink& audit;
  /// Optional approval broker that gates the `Verdict::ask` flow. When
  /// non-null *and* `approval_token` is non-null, `dispatch` consults
  /// `broker.check(token, name, input, identity, now)` after rule
  /// evaluation: success promotes the audit outcome to `approved` and
  /// the handler runs; failure demotes the outcome to `rejected` and
  /// the broker's error (with its `reason` context entry —
  /// `expired` / `tool_mismatch` / `identity_mismatch` /
  /// `input_mismatch` / `mac_mismatch` / `no_grant` /
  /// `replay_exhausted`) is forwarded to the caller. When either
  /// field is null, the legacy short-circuit applies — the audit
  /// outcome stays `ask` and the call returns
  /// `permission_denied` with `reason=approval_required`. The pointer
  /// is non-owning; the caller (typically the agent loop) keeps the
  /// broker alive across dispatch invocations.
  permission::ApprovalBroker* approval_broker{nullptr};
  /// Optional approval token. See `approval_broker` above for how
  /// `dispatch` consumes the pair. The pointer is non-owning; the
  /// caller keeps the token alive across the dispatch invocation.
  const permission::ApprovalToken* approval_token{nullptr};
  /// Wall-clock instant the broker uses to evaluate `expires_at`.
  /// The agent loop sets this from `core::time::now_utc()` per
  /// dispatch; tests pin it to a fixed time so the broker's TTL
  /// branches are deterministic. Default-constructed value (the
  /// UNIX epoch) is intentionally far in the past so that an
  /// uninitialised value cannot accidentally satisfy a real TTL
  /// — every realistic call site supplies a fresh value.
  core::Time now{};
  /// Optional hook bus. When non-null, `dispatch` publishes
  /// `hook::Event::tool_before` after the registry resolves the tool
  /// def (i.e., for every known tool name) and
  /// `hook::Event::tool_after` at every exit (handler success,
  /// permission denied, broker rejection, audit error). Hooks are
  /// advisory in this slice — sinks observe but cannot veto. The
  /// pointer is non-owning; the caller (typically the agent loop)
  /// keeps the bus alive across dispatch invocations. Failures
  /// reported by sinks are logged into the publish outcome but do
  /// not change the dispatch result.
  hook::Bus* bus{nullptr};
  /// Per-process scope key the audit row gets stamped with. See
  /// `docs/design-docs/secrets-and-state.md` "Identity And Scope".
  std::string scope_key;
  /// Agent making the call. Defaults to empty so unit tests that don't care
  /// about the agent identifier can stay terse.
  std::string agent_key;
  /// Operator / agent identity bound to the call. Carried into the audit row
  /// and (eventually) into the approval broker.
  std::string identity;
};

/// Handler signature. Handlers are coroutines that take the raw JSON input
/// the LLM produced and the per-call context, then return either a populated
/// `Output` or an `Error`. The JSON is intentionally passed as
/// `std::string_view` rather than a parsed `nlohmann::json` so this header
/// stays free of the nlohmann include — handlers parse with `nlohmann::json`
/// in their own TU.
using Handler =
    std::function<async::Awaitable<core::Result<Output>>(std::string_view input_json, DispatchContext& ctx)>;

class Registry {
public:
  Registry() = default;

  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;
  Registry(Registry&&) noexcept = default;
  Registry& operator=(Registry&&) noexcept = default;
  ~Registry() = default;

  /// Register `def` + `handler`. Returns `Error::conflict` if a tool with
  /// the same name is already registered, `Error::invalid_argument` if
  /// either piece is malformed (empty name, invalid `input_schema_json`, or
  /// null handler).
  [[nodiscard]] core::Result<void> add(core::ToolDef def, Handler handler);

  /// Remove the tool named `name`. Returns `Error::not_found` if no such
  /// tool was registered.
  [[nodiscard]] core::Result<void> remove(std::string_view name);

  /// Number of registered tools.
  [[nodiscard]] std::size_t size() const noexcept {
    return entries_.size();
  }

  /// Lookup a tool by name. Returns a pointer that is valid until the next
  /// `add` / `remove`; null when no tool matches.
  [[nodiscard]] const core::ToolDef* find(std::string_view name) const;

  /// All registered tool definitions, ordered by insertion. Useful for the
  /// agent-loop's "advertise this catalog to the provider" step.
  [[nodiscard]] std::vector<core::ToolDef> catalog() const;

  /// Run one tool. The flow is:
  ///
  ///   1. lookup `name`; `Error::not_found` on miss. No hook event
  ///      is published for an unknown tool name — the dispatch
  ///      never started.
  ///   2. if `ctx.bus` is non-null, publish
  ///      `hook::Event::tool_before` with `ToolBeforePayload{name,
  ///      input_json, identity, scope_key, agent_key, started_at}`.
  ///      Sink failures are recorded in the outcome but do not
  ///      change the dispatch path.
  ///   3. evaluate `(name, input_json, def.required_capabilities, ctx.mode)`
  ///      against `ctx.rules`.
  ///   4. if the verdict is `ask` and both `ctx.approval_broker` and
  ///      `ctx.approval_token` are set, consult
  ///      `broker.check(*token, name, input, identity, now)` and
  ///      remap the audit outcome to `approved` (broker accepted) or
  ///      `rejected` (broker rejected — the broker's `reason`
  ///      context entry replaces the rule reason in the audit row).
  ///   5. record one `permission::AuditEvent` carrying the final
  ///      verdict, the (possibly remapped) outcome,
  ///      `input_hash = SHA-256(input_json)`, and the identity
  ///      columns from `ctx`.
  ///   6. branch:
  ///        - `allow`           -> co_await `handler`, return its `Result`.
  ///        - `deny`            -> return `Error::permission_denied`
  ///                               with `reason=<rule_reason>`.
  ///        - `ask` (approved)  -> co_await `handler`, return its `Result`.
  ///        - `ask` (rejected)  -> return the broker's error verbatim
  ///                               (its `reason` context entry already
  ///                               classifies the failure).
  ///        - `ask` (no broker
  ///           or no token)     -> return `Error::permission_denied`
  ///                               with `reason=approval_required`,
  ///                               `decision_reason=<rule_reason>`,
  ///                               `replay_max=<decimal>`, and
  ///                               `approval_ttl_seconds=<decimal>`
  ///                               copied from the matched rule so
  ///                               the agent loop can hand them
  ///                               straight to
  ///                               `ApprovalBroker::approve`.
  ///   7. if `ctx.bus` is non-null, publish
  ///      `hook::Event::tool_after` with `ToolAfterPayload{name,
  ///      input_json, identity, succeeded, output_text, error_kind,
  ///      error_message, started_at, finished_at, duration}` —
  ///      always, regardless of which branch in step 6 fired.
  ///
  /// Audit-sink errors are propagated verbatim so a flaky storage backend
  /// does not silently lose decisions. Hook publish errors are *not*
  /// propagated to the caller (advisory contract).
  [[nodiscard]] async::Awaitable<core::Result<Output>>
  dispatch(std::string_view name, std::string_view input_json, DispatchContext& ctx) const;

private:
  struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view>{}(value);
    }

    [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
      return (*this)(std::string_view{value});
    }
  };

  struct Entry {
    core::ToolDef def;
    Handler handler;
    std::size_t insertion_index{0};
  };

  std::unordered_map<std::string, Entry, TransparentStringHash, std::equal_to<>> entries_;
  std::size_t next_index_{0};
};

}  // namespace orangutan::tool

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
//   4. The slice-21 + slice-94 approval-broker bridge: when a rule fires
//      `Verdict::ask` and the caller supplies a
//      `permission::ApprovalBroker` plus a `permission::ApprovalToken`
//      in the context, dispatch consults the broker, promotes the audit
//      outcome to `approved`/`rejected`, and either runs the handler or
//      forwards the broker's rejection verbatim. When a broker and bus are
//      present but no replay token was supplied, dispatch publishes blocking
//      `permission_ask_rendered`; a subscribed sink returning `proceed`
//      issues a fresh broker token, stores it in `approval_token_output`
//      when requested, and runs the handler after immediate broker.check.
//      With no token and no subscribed ask sink, the short-circuit
//      `approval_required` path is preserved — but the resulting `Error`
//      carries `replay_max` / `approval_ttl_seconds` / `decision_reason`
//      context entries so the agent loop can approve without re-evaluating
//      the rule set.
//   5. The slice-22+25+91+152 hook-bus tap: when the caller supplies a
//      `hook::Bus*` on the context, dispatch first publishes blocking
//      `hook::Event::tool_before`, consuming veto / rewrite /
//      require_approval decisions before permission evaluation. It then
//      publishes advisory `tool_dispatched` on paths where the handler will
//      run, `tool_error` on error exits, and `tool_after` at every exit.
//      Sensitive `FileWrite` / `FileEdit` payloads carry redacted
//      hash-and-byte-count input views for non-trusted hook sinks.
//      Sinks subscribed to other events (provider, memory, …) are unaffected.
//   6. The slice-55 workspace pre-resolution boundary: when a caller
//      supplies `DispatchContext::workspace`, dispatch resolves known
//      filesystem built-in `path` inputs before permission evaluation,
//      carries the absolute path to handlers via `ctx.resolved_path`, and
//      records redacted resolver metadata in the audit row.
//
// What this slice does NOT cover. Binding the CLI operator-prompt sink into
// the full binary agent-loop handoff, output scrubbing through
// `oran-log::redact`, deferred-tool promotion, and the future capability-
// gated `tool::Runtime::workspace()` accessor all live in later slices.
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
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/hook/bus.hpp>
#include <oran/io/directory_authority.hpp>
#include <oran/permission/approval.hpp>
#include <oran/permission/approval_broker.hpp>
#include <oran/permission/audit.hpp>
#include <oran/permission/rule_set.hpp>
#include <oran/tool/output.hpp>

namespace orangutan::tool {

class Registry;
class Workspace;
struct DispatchContext;

/// Runtime callback consumed by the `SkillInvoke` built-in. The tool layer
/// owns JSON parsing, permissions, audit, hooks, and output caps; bootstrap
/// supplies the already-loaded skill snapshot through this callback so
/// `oran-tool` does not depend on `oran-skill`.
using SkillInvokeHandler = std::function<async::Awaitable<core::Result<Output>>(std::string_view skill_name,
                                                                                std::string_view inputs_json,
                                                                                DispatchContext& ctx)>;

/// Runtime callback consumed by the `SkillDeactivate` built-in. Like
/// `SkillInvokeHandler`, the tool layer owns JSON parsing, permissions, audit,
/// hooks, and output caps; bootstrap supplies the already-loaded skill snapshot
/// through this callback so `oran-tool` does not depend on `oran-skill`.
/// Deactivation carries no `inputs`, so the signature is the skill name only.
using SkillDeactivateHandler =
    std::function<async::Awaitable<core::Result<Output>>(std::string_view skill_name, DispatchContext& ctx)>;

struct MemoryRecallRequest {
  std::string query;
  std::size_t limit{5};
  std::vector<std::string> kinds;

  friend bool operator==(const MemoryRecallRequest&, const MemoryRecallRequest&) = default;
};

struct MemoryRememberRequest {
  std::string id;
  std::string kind;
  std::string title;
  std::string body;
  double importance{0.5};
  std::vector<std::string> tags;
  std::vector<std::string> linked_record_ids;
  bool shadow{false};

  friend bool operator==(const MemoryRememberRequest&, const MemoryRememberRequest&) = default;
};

struct MemoryForgetRequest {
  std::string id;

  friend bool operator==(const MemoryForgetRequest&, const MemoryForgetRequest&) = default;
};

/// Runtime callback consumed by the `MemoryRecall` built-in. The tool layer
/// owns JSON parsing, permissions, audit, hooks, and output caps; bootstrap
/// supplies the concrete long-term memory runtime through this callback so
/// `oran-tool` does not take a sibling dependency on `oran-memory`.
using MemoryRecallHandler =
    std::function<async::Awaitable<core::Result<Output>>(MemoryRecallRequest request, DispatchContext& ctx)>;

/// Runtime callback consumed by the `MemoryRemember` built-in. The tool layer
/// owns JSON parsing, permissions, audit, hooks, and output caps; bootstrap
/// supplies the concrete long-term memory backend through this callback so
/// `oran-tool` does not take a sibling dependency on `oran-memory`.
using MemoryRememberHandler =
    std::function<async::Awaitable<core::Result<Output>>(MemoryRememberRequest request, DispatchContext& ctx)>;

/// Runtime callback consumed by the `MemoryForget` built-in. The tool layer
/// owns JSON parsing, permissions, audit, hooks, and output caps; bootstrap
/// supplies the concrete long-term memory backend through this callback so
/// `oran-tool` does not take a sibling dependency on `oran-memory`.
using MemoryForgetHandler =
    std::function<async::Awaitable<core::Result<Output>>(MemoryForgetRequest request, DispatchContext& ctx)>;

/// Registry-pre-resolved filesystem target for a built-in tool call. The
/// `authority` plus `relative_path` is the executable capability for migrated
/// handlers. It is optional only during the staged migration so legacy direct
/// registry callers remain representable; migrated handlers must reject a
/// missing authority. `absolute_path` remains non-authoritative compatibility
/// metadata until the remaining filesystem built-ins migrate.
struct ResolvedToolPath {
  std::optional<io::DirectoryAuthority> authority{};
  std::string authority_relative_path;
  std::string absolute_path;
  std::string relative_path;
  /// Stable label used in audit metadata. For ordinary workspace paths this is
  /// `<workspace>/...` or an extra-root label; for per-call outside overrides
  /// this is the resolved absolute path.
  std::string display_path;
  std::string input_path_hash;
  std::string workspace_root_hash;
  bool symlink_followed{false};
  bool created_parents{false};
  /// True for configured extra-root matches and per-call outside read/list
  /// overrides.
  bool outside_workspace_explicit_override{false};
  bool per_call_outside_workspace_override{false};
  std::optional<std::size_t> override_root_index{};
};

/// Per-call context the registry threads into every handler. Holds references
/// to the long-lived permission infrastructure plus the per-call identity
/// strings the audit pipeline needs.
///
/// The struct is non-copyable / non-movable (it holds references); the caller
/// brace-initialises one on the stack per `dispatch` invocation.
struct DispatchContext {
  /// Create a fresh context for the current wall clock. This is the
  /// production default for callers that do not need a pinned broker clock;
  /// tests that need deterministic approval TTL behavior can still aggregate
  /// initialise the struct and set `now` explicitly.
  [[nodiscard]] static DispatchContext for_now(asio::any_io_executor executor,
                                               permission::RuleSet& rules,
                                               permission::AuditSink& audit,
                                               std::string scope_key = {},
                                               std::string agent_key = {},
                                               std::string identity = {});

  /// Clone a caller-owned prototype for one dispatch, refreshing `now` and
  /// clearing fields that `Registry::dispatch` mutates. `approval_token_output`
  /// is copied only when the caller can guarantee the output slot is not
  /// shared by concurrent dispatches.
  [[nodiscard]] static DispatchContext for_now(const DispatchContext& prototype, bool thread_approval_token_output);

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
  /// field is null, dispatch may publish blocking
  /// `hook::Event::permission_ask_rendered` when `bus` is also present.
  /// A subscribed sink returning `proceed` issues a fresh broker token and
  /// can copy it to `approval_token_output`; a `veto` returns
  /// `permission_denied` with `reason=operator_denied`. With no subscribed
  /// ask sink, the legacy short-circuit applies — the audit outcome stays
  /// `ask` and the call returns `permission_denied` with
  /// `reason=approval_required`. The pointer is non-owning; the caller
  /// (typically the agent loop) keeps the broker alive across dispatch
  /// invocations.
  permission::ApprovalBroker* approval_broker{nullptr};
  /// Optional approval token. See `approval_broker` above for how
  /// `dispatch` consumes the pair. The pointer is non-owning; the
  /// caller keeps the token alive across the dispatch invocation.
  const permission::ApprovalToken* approval_token{nullptr};
  /// Optional output slot for approvals issued after a blocking
  /// `permission_ask_rendered` prompt. When a sink approves an ask and no
  /// caller-supplied `approval_token` was present, `dispatch` stores the
  /// freshly issued token here before checking it through the broker. The
  /// caller may keep the token for identical-input replay until the broker
  /// exhausts or expires the grant.
  permission::ApprovalToken* approval_token_output{nullptr};
  /// Wall-clock instant the broker uses to evaluate `expires_at`.
  /// The agent loop sets this from `core::time::now_utc()` per
  /// dispatch; tests pin it to a fixed time so the broker's TTL
  /// branches are deterministic. Default-constructed value (the
  /// UNIX epoch) is intentionally far in the past so that an
  /// uninitialised value cannot accidentally satisfy a real TTL
  /// — every realistic call site supplies a fresh value.
  core::Time now{};
  /// Optional hook bus. When non-null, `dispatch` publishes blocking
  /// `hook::Event::tool_before` after the registry resolves the tool def
  /// (i.e., for every known tool name) and consumes veto / rewrite /
  /// require_approval decisions before workspace resolution and permission
  /// evaluation. It then publishes advisory `tool_dispatched` before
  /// handlers run, `tool_error` on failures, and `tool_after` at every
  /// exit. The pointer is non-owning; the caller (typically the agent loop)
  /// keeps the bus alive across dispatch invocations.
  hook::Bus* bus{nullptr};
  /// Registry currently running this dispatch. Set by `Registry::dispatch`
  /// before any handler runs and restored when dispatch exits. Most handlers
  /// ignore it; metadata tools such as `ToolSearch` use it to inspect the
  /// live catalog without capturing a self-reference inside a movable
  /// `Registry`.
  const Registry* registry{nullptr};
  /// Optional skill invocation service. When set, `SkillInvoke` calls it with
  /// the requested skill name plus the raw `inputs` JSON value and returns the
  /// produced tool output through the ordinary dispatch path. When unset,
  /// `SkillInvoke` reports a model-repairable missing-runtime error.
  SkillInvokeHandler skill_invoke{};
  /// Optional skill deactivation service. When set, `SkillDeactivate` calls it
  /// with the requested skill name and returns the produced tool output through
  /// the ordinary dispatch path; the runner records a versioned deactivation
  /// record in the result `data_json` so the next prompt boundary clears the
  /// active marker. When unset, `SkillDeactivate` reports a model-repairable
  /// missing-runtime error.
  SkillDeactivateHandler skill_deactivate{};
  /// Optional long-term memory recall service. When set, `MemoryRecall` calls
  /// it with the parsed query, limit, and kind spellings, and returns the
  /// produced output through the ordinary dispatch path. When unset,
  /// `MemoryRecall` reports a model-repairable missing-runtime error.
  MemoryRecallHandler memory_recall{};
  /// Optional long-term memory write service. When set, `MemoryRemember` calls
  /// it with parsed record fields and returns the produced output through the
  /// ordinary dispatch path. When unset, `MemoryRemember` reports a
  /// model-repairable missing-runtime error.
  MemoryRememberHandler memory_remember{};
  /// Optional long-term memory delete service. When set, `MemoryForget` calls
  /// it with the parsed record id and returns the produced output through the
  /// ordinary dispatch path. When unset, `MemoryForget` reports a
  /// model-repairable missing-runtime error.
  MemoryForgetHandler memory_forget{};
  /// Optional workspace resolver for file built-ins. The pointer is
  /// non-owning; bootstrap/agent runtime owns the workspace value and keeps it
  /// alive for the dispatch. Dispatch pre-resolves current filesystem
  /// built-ins through this seam before permission evaluation and stores the
  /// result in `resolved_path`. FileWrite and FileEdit require this authority;
  /// direct callers of those mutation tools must provide a workspace.
  Workspace* workspace{nullptr};
  /// Resolved path for the currently executing filesystem built-in. Set by
  /// `Registry::dispatch` after `tool_before` and before permission
  /// evaluation when `workspace` is supplied and the target tool is a known
  /// filesystem built-in. Cleared on every dispatch entry so callers can
  /// reuse a context object safely.
  std::optional<ResolvedToolPath> resolved_path{};
  /// Output byte caps applied after a successful handler return and before
  /// hook/provider-facing output leaves the dispatch boundary. The future
  /// scheduler owns these options for batched calls; direct registry callers
  /// get the spec-0014 defaults.
  OutputCapOptions output_caps{};
  /// Optional parent turn id supplied by `agent::Loop` when trace correlation
  /// is enabled. `Registry::dispatch` copies it into `permission::AuditEvent`
  /// so tool audit rows can join to `trace_turns.turn_id`.
  std::optional<core::TurnId> parent_turn_id{};
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
  ///   2. if `ctx.bus` is non-null, publish blocking
  ///      `hook::Event::tool_before` with `ToolBeforePayload{name,
  ///      input_json, identity, scope_key, agent_key, started_at}`.
  ///      A veto records `outcome=blocked_by_hook`, skips the handler,
  ///      publishes the advisory failure events, and returns
  ///      `Error::permission_denied`. A rewrite substitutes the effective
  ///      input before workspace resolution, permission evaluation, broker
  ///      checks, audit, handler execution, and later hook payloads. A
  ///      require_approval decision promotes an otherwise-allow verdict into
  ///      the existing broker path.
  ///   3. if `ctx.workspace` is supplied and `name` is one of the built-in
  ///      filesystem tools, pre-resolve its `path` field through the
  ///      intent-specific `Workspace` method, stash the absolute path in
  ///      `ctx.resolved_path`, and carry redacted resolver metadata for audit.
  ///      A resolver failure is kept as the pending dispatch result.
  ///   4. evaluate `(name, effective_input, def.required_capabilities, ctx.mode)`
  ///      against `ctx.rules` so audit still records the permission context
  ///      for known filesystem attempts that fail path policy.
  ///   5. if the verdict is `ask` and no path-resolution failure is pending:
  ///      - if both `ctx.approval_broker` and `ctx.approval_token` are set,
  ///        consult `broker.check(*token, name, effective_input, identity,
  ///        now)` and remap the audit outcome to `approved` (broker
  ///        accepted) or `rejected` (broker rejected — the broker's `reason`
  ///        context entry replaces the rule reason in the audit row);
  ///      - else if `ctx.approval_broker` and `ctx.bus` are set, publish
  ///        blocking `hook::Event::permission_ask_rendered`. A subscribed
  ///        sink returning `proceed` issues a broker token, optionally stores
  ///        it in `approval_token_output`, checks it immediately, and runs the
  ///        handler on success. A `veto` records `outcome=rejected` and
  ///        returns `permission_denied` with `reason=operator_denied`.
  ///        With no subscribed ask sink, the legacy `approval_required`
  ///        short-circuit below is preserved.
  ///   6. record one `permission::AuditEvent` carrying the final
  ///      verdict, the (possibly remapped) outcome,
  ///      `input_hash = SHA-256(effective_input)`, the identity columns from
  ///      `ctx`, and any resolver / blocking-hook metadata under
  ///      `metadata_json` (`hook_decisions`, `original_input_hash`,
  ///      `rewritten_input_hash` when applicable).
  ///   7. branch:
  ///        - resolver failure -> return the resolver error; the handler is
  ///                              not run and approval replay is not spent.
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
  ///   8. if `ctx.bus` is non-null, publish `hook::Event::tool_error`
  ///      on failures, then always publish `hook::Event::tool_after` with
  ///      `ToolAfterPayload{name, effective_input, identity, succeeded,
  ///      output_text, error_kind, error_message, started_at, finished_at,
  ///      duration}` regardless of which branch in step 6 fired.
  ///
  /// Audit-sink errors are propagated verbatim so a flaky storage backend
  /// does not silently lose decisions. Advisory hook publish errors are not
  /// propagated to the caller; blocking `tool_before` sink errors are
  /// converted by the bus into a hook veto.
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

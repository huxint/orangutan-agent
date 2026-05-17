// include/oran/tool/registry.hpp — first-pass tool registry surface.
//
// Slice 17 of the v2 stack. The registry composes three things the upstream
// layers (`oran-permission`, `oran-async`, `oran-io`) already shipped:
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
//
// What this slice does NOT cover. The hook bus, the `Verdict::ask` approval
// flow via `permission::ApprovalBroker`, output scrubbing through
// `oran-log::redact`, and deferred-tool promotion all live in later slices.
// `Verdict::ask` is recorded faithfully in audit (`AuditOutcome::ask`) and
// short-circuits with `permission_denied`/`reason=approval_required` until
// the broker is wired through here.
//
// Why a single `DispatchContext` rather than the design-doc's parameter
// fan-out. Passing `permission::RuleSet&`, `permission::AuditSink&`,
// `permission::Mode`, scope/agent/identity, and the executor as separate
// arguments to `dispatch` and to every handler quickly turns the
// `Handler` signature into a maintenance hazard. A typed struct makes
// it cheap to add the hook bus or the approval broker later — handlers
// keep the same signature; new context fields are additive.
//
// Concurrency. Like `permission::AuditSink`, the registry is not
// thread-safe. The agent loop owns one registry per strand; a future
// concurrent caller wraps the registry in an `asio::strand` rather than
// reaching for an internal mutex (the legacy code's per-member mutex
// pattern was a recurring source of contention — see
// `docs/references/orangutan-legacy-audit.md`).

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/core/tool_def.hpp>
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
  /// either piece is malformed (empty name or null handler).
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
  ///   1. lookup `name`; `Error::not_found` on miss.
  ///   2. evaluate `(name, input_json, def.required_capabilities, ctx.mode)`
  ///      against `ctx.rules`.
  ///   3. record one `permission::AuditEvent` carrying the decision,
  ///      `input_hash = SHA-256(input_json)`, and the identity columns
  ///      from `ctx`.
  ///   4. branch:
  ///        - `allow`  -> co_await `handler`, return its `Result`.
  ///        - `deny`   -> return `Error::permission_denied`.
  ///        - `ask`    -> return `Error::permission_denied` with
  ///                       `reason=approval_required` (approval flow
  ///                       lands in a later slice).
  ///
  /// Audit-sink errors are propagated verbatim so a flaky storage backend
  /// does not silently lose decisions.
  [[nodiscard]] async::Awaitable<core::Result<Output>>
  dispatch(std::string_view name, std::string_view input_json, DispatchContext& ctx) const;

private:
  struct Entry {
    core::ToolDef def;
    Handler handler;
    std::size_t insertion_index{0};
  };

  std::unordered_map<std::string, Entry> entries_;
  std::size_t next_index_{0};
};

}  // namespace orangutan::tool

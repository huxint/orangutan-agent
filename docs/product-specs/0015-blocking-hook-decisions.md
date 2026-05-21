# 0015 — Blocking Hook Decisions

## User Problem

Today's hook bus is advisory-only — sinks observe lifecycle events but
cannot veto, rewrite, or short-circuit the dispatch. The design doc
already enumerates a `blocking` mode and an `EventTraits<E>::Decision`
shape ([`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)
"Synchronous vs. Async Hooks"); the tracker carries the row as
*deferred until the first blocking consumer exists*. Six concrete
behaviours are blocked until that consumer ships:

- **Operator approval rendering.** `permission_ask_rendered` is the
  hook that the agent loop uses to ask a human "may I run this tool?"
  An advisory publish lets the sink observe the question; it cannot
  return the answer. The render-side approval flow is therefore stuck
  in the agent-loop slice waiting on this primitive.
- **Tool-input rewrite.** A `tool_before` sink that wants to redact a
  path, narrow a glob, or substitute a safer arg list has no way to
  return a new input. Today the dispatch sees the original input only.
- **Hook-driven veto.** A policy sink (e.g. a corporate firewall hook
  that blocks all writes outside `src/`) has no way to short-circuit
  beyond crashing — and crashing an advisory sink only produces a log
  line, not a `permission_denied`.
- **Memory-write gating.** `memory_write_before` is the natural place
  for a sink to reject a memory write that contains PII. Same blocker.
- **Provider request rewrite.** `provider_request` is the seam where a
  trace sink could inject sampling headers or strip sensitive content.
  Same blocker.
- **Hook timeout / failure policy.** A blocking sink can hang or crash;
  the bus needs explicit timeout, failure classification, and audit
  semantics for both. None exist today because the publish path is
  fire-and-forget.

The deep-review §5.3 reinforces this priority: *do not build the
render-side ask flow in `oran-agent` as a special case; make it the
first consumer of `publish_blocking`.* Otherwise the agent loop bakes
in render-specific assumptions that block every later blocking sink
from re-using the same path.

This spec gives the blocking-hook surface a product-side home. The
implementation lives in `oran-hook` (design doc already names the
field `publish_blocking`); this spec is what ships, what's tested, and
in what order.

## Scope (v1)

The MVP is the *minimum* surface needed by the agent loop's approval
render flow — the first real consumer. Everything else that wants a
blocking hook waits for v1.1.

- **`hook::Bus::publish_blocking<E>(Payload) -> Awaitable<Result<HookDecision<E>>>`**
  ([`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)
  "Synchronous vs. Async Hooks" already sketches the `EventTraits<E>::Decision`
  pattern; this spec pins the user-facing semantics):
  ```cpp
  enum class HookDecisionKind {
    proceed,         // run handler with original input
    veto,            // do not run handler; treat as permission_denied
    rewrite,         // run handler with HookDecision::rewritten_input
    require_approval // route through ApprovalBroker before running
  };

  struct HookDecision {
    HookDecisionKind                 kind{HookDecisionKind::proceed};
    std::string                      reason;            // free-form, audited
    std::optional<nlohmann::json_fwd> rewritten_input;  // required iff kind==rewrite
    std::optional<core::Time>        approval_expires_at; // optional, kind==require_approval only
  };
  ```
  Compile budget: `nlohmann::json_fwd` only in the public header per
  [`../rules/compile-budget.md`](../rules/compile-budget.md); the full
  include lives in `src/oran-hook/bus.cpp`.
- **Whitelist of blocking events** (v1 — the rest fall back to
  advisory until a consumer asks):
  - `tool_before` — veto / rewrite / require_approval.
  - `permission_ask_rendered` — proceed / veto (renders the prompt; the
    sink's `reason` carries the operator's response identity).
  - `memory_write_before` — veto / proceed (rewrite deferred to v1.1).
  Every other event in `hook::Event` stays advisory in v1.
- **Sink resolution order**. Blocking sinks for the same event execute
  in subscription order. The first non-`proceed` decision short-circuits
  later sinks for that publish. The bus records every sink's decision
  in the audit row, not just the winning one.
- **Veto / rewrite audit.** A `tool_before` rewrite records both the
  original `input_hash` and the rewritten `input_hash` in the
  `AuditEvent` context map; the existing `input_hash` SHA-256 discipline
  (`Registry::dispatch`) covers both. The outcome flips from `allow` to
  `rewritten` (new enumerator in `permission::AuditOutcome`) when a
  rewrite fires; a veto flips it to `blocked_by_hook` (new enumerator);
  a `require_approval` decision flips it to the existing `ask` /
  `approved` / `rejected` triad once the broker resolves.
- **Hook timeout**. `config.hooks.timeout_ms` (default 2000, per the
  existing design doc). A blocking sink that exceeds the timeout
  returns `Error::HookTimeout`; the dispatch treats this as a veto
  with `reason=timeout`. Audit records `blocked_by_hook`,
  `reason=hook_timeout`, and the offending sink's id.
- **Hook failure**. A blocking sink that returns an `Error` is treated
  as a veto with `reason=hook_error`; the underlying error is forwarded
  to the audit context. A sink that throws is captured by the bus
  (advisory contract already does this in `src/oran-hook/bus.cpp:54-81`)
  and treated identically to a returned error.
- **Order with the existing dispatch pipeline.** The canonical order
  becomes:
  ```
  1. tool_before (blocking)     <- this spec adds the blocking phase
     proceed / rewrite / veto / require_approval
  2. permission engine evaluates against {tool, input, identity, caps}
  3. if ask -> ApprovalBroker (existing flow)
  4. tool_dispatched (advisory)
  5. handler runs
  6. tool_after (advisory)
  ```
  Phase 1's `rewrite` substitutes the input *before* permission
  evaluation so a rewriter cannot accidentally smuggle past a
  permission rule. `require_approval` causes phase 3 to fire even when
  no `ask` rule matched — the hook overrides the verdict upward, never
  downward.
- **Consumer #1**: the agent loop's approval render flow. The first
  in-tree `permission_ask_rendered` sink is owned by `oran-cli` (and
  later `oran-web` / `oran-channel*`) and lives behind the same
  `tool::DispatchContext::bus` field that slice 22 wired. The agent
  loop never sees rendering details.

## Scope (v1.1)

- Blocking on `memory_write_before` *rewrite* mode.
- Blocking on `provider_request` rewrite (sampling / header injection).
- Blocking on `provider_response` redact (drop PII fields before they
  hit the prompt cache).
- Sink-side capabilities declaration: `Sink::capabilities()` returns
  `{ may_block, may_rewrite, may_require_approval }`. The bus rejects
  subscription to a blocking event when the sink does not declare
  `may_block`.
- Parallel sink fan-out for advisory publishes (tracked separately in
  the deep-review backlog, but this spec depends on it not being a
  prerequisite — advisory publishes stay sequential in v1).

## Scope (v2)

- Approval routing to external channels (Slack, email) — implemented as
  a `permission_ask_rendered` sink that delegates over `oran-channel`.
  Already in spec 0008 v2.
- Wasm sink with sandboxed blocking decisions
  (`docs/design-docs/permissions-and-hooks.md` "Sink Kinds" notes
  WasmSink as stretch). The blocking contract on a sandboxed sink is
  the same; only the implementation differs.

## Out Of Scope

- Blocking on advisory-by-design events (`tool_after`, `iteration_end`,
  `final_response`). Their semantics are *the dispatch already ran*; a
  veto after the fact is incoherent. The bus type-system enforces this
  through `EventTraits<E>::Decision = void` for advisory events
  (already documented in the design doc).
- Cross-sink decision negotiation. Two blocking sinks return different
  decisions → first non-`proceed` wins. A "consensus" or "voting" mode
  is out of scope; sink ordering is the operator's responsibility.

## Acceptance Criteria

1. **`tool_before` veto.** A sink subscribed to `tool_before` that
   returns `HookDecision{ .kind=veto, .reason="policy" }` causes
   `Registry::dispatch` to skip the handler, record an `AuditEvent`
   with `outcome=blocked_by_hook` and `reason=policy`, publish exactly
   one `tool_after` payload with `succeeded=false` and
   `error_kind=blocked_by_hook`, and return
   `Error::permission_denied` to the agent loop. Pinned by a
   regression test.
2. **`tool_before` rewrite.** A rewrite sink that returns
   `HookDecision{ .kind=rewrite, .rewritten_input=... }` causes the
   permission engine to evaluate against the rewritten input, the
   handler to receive the rewritten input, the `AuditEvent`'s
   `context` map to carry both `input_hash` and
   `rewritten_input_hash`, and the `outcome` to be `rewritten`
   (followed by the verdict on the rewritten input). The agent
   transcript records the rewritten input, never the original.
3. **`tool_before` require_approval.** A sink that returns
   `HookDecision{ .kind=require_approval }` causes
   `Registry::dispatch` to consult the broker exactly as if a
   `Verdict::ask` rule had matched, even when the permission engine
   would otherwise have returned `allow`. Approval succeeds →
   `outcome=approved`; approval fails → `outcome=rejected`.
4. **`permission_ask_rendered` round-trip.** When a `Verdict::ask`
   rule matches, the dispatch pipeline publishes
   `permission_ask_rendered` *blocking*; the sink's `HookDecision`
   carries the operator's response identity in `reason` (e.g.
   `reason=operator_approved:huxint`); the broker accepts the
   resulting `ApprovalToken` and resumes dispatch. A
   `kind=veto` decision aborts with `outcome=rejected,
   reason=operator_denied`.
5. **Sink ordering**. Three blocking sinks subscribed to the same
   event execute in subscription order; the first non-`proceed`
   short-circuits the rest. All three decisions are recorded in
   `AuditEvent::context.hook_decisions` (a JSON array of
   `{sink_id, kind, reason}`). Pinned by a multi-sink test.
6. **Timeout.** A sink whose handler awaits longer than
   `config.hooks.timeout_ms` causes the dispatch to abort with
   `outcome=blocked_by_hook, reason=hook_timeout`; the
   `AuditEvent::context` records the offending `sink_id` and
   `elapsed_ms`. Tested against a controllable-latency fake sink.
7. **Sink error.** A blocking sink returning `Error::internal` is
   treated as veto with `reason=hook_error`; the underlying error
   message rides in `AuditEvent::context.error`.
8. **Advisory unchanged.** Existing advisory publishes
   (`tool_after`, `iteration_end`, etc.) maintain their
   `publish_advisory` contract — sinks observe, no decision is
   honoured, no dispatch flow change. Regression-test asserts that
   `publish_advisory` bytes-on-the-wire equal pre-spec.
9. **Type safety.** `publish_blocking<Event::tool_after>` fails to
   compile (no `EventTraits<tool_after>::Decision`). Pinned by a
   compile-fail test under the existing `tests/compile-fail` harness
   pattern.
10. **`tests/hook/`** ≥ 90% coverage of the matrix (event × decision
    kind × sink ordering × timeout × error).
11. **`bench/oran-hook/publish_blocking_overhead`** reports a
    single-sink blocking publish cost ≤ 2× the single-sink advisory
    publish baseline (~446 ns per `bench-hook/publish_one_sink`), and
    a no-sink blocking publish ≤ 1.5× the no-sink advisory baseline.

## Design Doc Cross-References

- [`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)
  — owns the field shapes (`Event`, `Sink`, `EventTraits<E>::Decision`,
  blocking/advisory annotation per event). This spec is the
  product-side counterpart that says *what the user gets* and *which
  consumers ship first*.
- [`0008-permissions.md`](0008-permissions.md) — `permission_ask_rendered`
  is the bridge between this spec and the existing approval-broker
  flow; the broker is unchanged, only the rendering path becomes
  blocking-hook-driven.
- [`0012-tool-scheduler-and-state.md`](0012-tool-scheduler-and-state.md)
  — the scheduler is the dispatch-time caller of `publish_blocking`;
  it owns the per-call cancellation slot and the timeout enforcement.
- [`0014-structured-tool-output.md`](0014-structured-tool-output.md) —
  the `HookDecision::rewritten_input` shape uses the same forward-
  declared `nlohmann::json_fwd` as `Output::data` to keep the public
  header compile-cheap.
- [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md)
  "Permission Ordering" — the canonical order moves to the seven-step
  list above; the doc-side edit lands in the same slice as v1.

## Risks

- **Wrong consumer leads to wrong contract.** If the operator-prompt
  sink is the *only* blocking consumer for too long, the contract may
  bake in render-specific assumptions. Mitigation: spec acceptance
  criteria #2 (rewrite) and #3 (require_approval) are testable with
  fake sinks before the operator-prompt sink ships; ship both with the
  same slice that lands `publish_blocking`.
- **Cancellation in flight.** A blocking sink hanging on operator
  input must be cancellable when the agent loop's parent token fires.
  Mitigation: the dispatch passes the parent cancellation slot into
  the publish; the bus surfaces cancellation as `Error::cancelled`
  with `outcome=blocked_by_hook, reason=cancelled`, distinct from
  timeout. Pinned by a cancellation regression test alongside the
  scheduler tests in spec 0012.
- **Audit volume.** Recording every sink's decision can explode the
  `audit.db` row size. Mitigation: the `hook_decisions` array is
  bounded by sink count (typically ≤ 3); audit rows above
  `audit.max_context_bytes` (new config knob, default 16 KiB) drop
  hook-decision detail and record `hook_decisions_truncated=true`.
- **Type-system overhead.** `EventTraits` specialisations grow with
  every blocking event. Mitigation: the v1 list is three events; new
  blocking events require a `Decision` type definition + a `default
  mode` annotation update, both single-line edits. No template
  metaprogramming beyond the existing pattern.

## Validation

```sh
xmake build oran-hook oran-tool
xmake test test-hook                    # decision matrix + ordering + timeout + error
xmake test test-tool                    # dispatch order with blocking hook
xmake build bench-oran-hook
xmake run bench-oran-hook publish_blocking_overhead
xmake run orangutan -- --explain-rules  # rule-side unchanged; hook bindings surface
```

## Out-of-Band Cross-Cuts

- `docs/design-docs/permissions-and-hooks.md` "Synchronous vs. Async
  Hooks" gains the `publish_blocking` signature and the v1 whitelist;
  the design-doc edit lands in the same slice as v1.
- `docs/design-docs/tool-runtime.md` "Permission Ordering" updates to
  the seven-step canonical order.
- `docs/exec-plans/tech-debt-tracker.md` — the 2026-05-18 hook bus row
  closes when v1 ships; its `Why It Exists` paragraph already names
  this spec's consumer (`permission_ask_rendered`) as the unblocking
  event.
- `docs/SECURITY.md` — gains a "Hook-driven veto" subsection citing
  spec 0015 once v1 ships, so the workspace-confinement claim (spec
  0013) and the hook-veto claim sit side-by-side.

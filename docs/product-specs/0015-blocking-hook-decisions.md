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

> **Status (slice 96, 2026-05-25):** the bus surface, the
> `tool_before` dispatch consumer, the configured blocking-timeout
> policy, and traced direct-dispatch `hook_publish` audit rows are
> shipped.
> `<oran/hook/decision.hpp>` exports `HookDecisionKind { proceed, veto,
> rewrite, require_approval }` and `HookDecision { kind, reason,
> optional<string> rewritten_input_json, optional<core::Time>
> approval_expires_at, vector<HookDecisionTrace> trace }` — the public
> header stays `nlohmann`-free
> (`rewritten_input_json` carries serialised JSON bytes, the same
> envelope as `ToolBeforePayload::input_json` and
> `tool::Output::data_json`). `<oran/hook/event_traits.hpp>` exports the
> empty primary `EventTraits<E>` template plus explicit specialisations
> for the v1 whitelist (`tool_before`, `permission_ask_rendered`,
> `memory_write_before`) and the `HasBlockingDecision<E>` concept that
> constrains `Bus::publish_blocking<E>`. `hook::Sink` grows a virtual
> `handle_blocking(Event, Payload) -> Awaitable<Result<HookDecision>>`
> whose default body returns `proceed`; `hook::InProcessSink` adds an
> optional `BlockingCallback` via `set_blocking_handler` so tests and
> in-tree sinks can opt in without breaking existing constructors.
> `Bus::publish_blocking<E>` walks subscribed sinks in subscription
> order, applies the same per-sink redaction the advisory path uses,
> short-circuits at the first non-`proceed` decision, and converts sink
> `core::Result` errors / thrown exceptions into a veto with
> `reason="hook_error: <message> [sink=<id>]"`. Slice 92 adds
> `hook::BusOptions { blocking_timeout }` (default 2000 ms, matching
> `config.hooks.timeout_ms`) and races each blocking sink against
> `async::sleep_for`; a timeout synthesizes a veto with
> `reason="hook_timeout"` and fills `HookDecisionTrace::elapsed`.
> `test-hook` now reports 30 cases / 207 assertions. Slice 91 adds the
> `HookDecisionTrace`
> vector so dispatch can serialize every consulted sink decision into
> audit metadata; `Registry::dispatch` now calls
> `publish_blocking<Event::tool_before>` before workspace resolution and
> permission evaluation, consumes veto/rewrite/require_approval
> decisions, and persists the audit-side
> `AuditOutcome::blocked_by_hook` / `rewritten` enumerators. Slice 92
> also threads `config.hooks.timeout_ms` through `RuntimeAssembly` into
> the assembly-owned `hook::Bus` and records timeout decisions in
> `metadata_json.hook_decisions[].elapsed_ms`.
> `test-tool` now reports 173 cases / 1739 assertions. Slice 93 adds
> the spec-0018 AC5 direct-dispatch `hook_publish` audit-row writer for
> traced blocking `tool_before` publishes. Slice 94 adds the
> direct-dispatch `permission_ask_rendered` bridge: the hook payload variant
> carries `PermissionAskRenderedPayload`, dispatch publishes it for `ask`
> decisions with a broker+bus and no replay token, `proceed` issues/checks a
> broker grant and may copy the token to `DispatchContext::approval_token_output`,
> `veto` records `operator_denied`, and no-sink buses preserve the legacy
> `approval_required` short-circuit. `test-tool` now reports 178 cases / 1838
> assertions. Slice 95 adds the concrete user-visible sink:
> `cli::OperatorPromptSink` renders `PermissionAskRenderedPayload` in the
> terminal, accepts yes/approve/proceed or no/deny/reject answers, returns
> `operator_approved:<identity>` / `operator_denied:<identity>` through the
> blocking decision trace, and has scripted-answer coverage for
> noninteractive tests. `test-cli` reports 10 cases / 68 assertions. Slice 96
> pins the first agent-loop consumer of that direct-dispatch bridge:
> `agent::Loop` refreshes `DispatchContext::now` around every direct tool
> dispatch, so `PermissionAskRenderedPayload::requested_at`, broker grant
> expiry, and immediate broker verification use the per-call wall clock even
> when the caller's reusable context held a stale value. `test-agent` covers a
> fake-provider turn whose `file.read` ask flows through
> `permission::ApprovalBroker` plus a blocking `permission_ask_rendered` sink,
> records `metadata_json.permission_ask_decisions[]`, returns the approved tool
> result to the provider, and verifies the issued token against the prompt time.

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
    rewrite,         // run handler with HookDecision::rewritten_input_json
    require_approval // route through ApprovalBroker before running
  };

  struct HookDecision {
    HookDecisionKind                 kind{HookDecisionKind::proceed};
    std::string                      reason;                  // free-form, audited
    std::optional<std::string>       rewritten_input_json;    // required iff kind==rewrite
    std::optional<core::Time>        approval_expires_at;     // optional, kind==require_approval only
    std::vector<HookDecisionTrace>   trace;                   // consulted sinks in order
  };
  ```
  **Status (slice 91, shipped):** the value types ship as shown above.
  `rewritten_input_json` is the serialised JSON envelope (matching
  `ToolBeforePayload::input_json` / `tool::Output::data_json`) so the
  public header avoids `<nlohmann/json.hpp>` per critical rule C6;
  consumers that want a structured form parse the bytes inside their
  own TU. `trace` is bus-owned metadata: each `HookDecisionTrace`
  carries `{sink_id, kind, reason}` for a sink the bus actually
  consulted before the first non-`proceed`. The spec sketch's
  `std::optional<nlohmann::json_fwd>` was a shorthand —
  `std::optional<T>` requires a complete type, so the shipped shape
  carries bytes instead of a fwd-decl handle.
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
  existing design doc). A blocking sink that exceeds the timeout is
  cancelled by the bus race and reported as a synthesized veto with
  `reason=hook_timeout`. Audit records `blocked_by_hook`,
  `reason=hook_timeout`, the offending sink's id, and `elapsed_ms`.
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
   regression test. **Status (slice 91):** shipped. `Registry::dispatch`
   consumes the veto decision, records `AuditOutcome::blocked_by_hook`,
   serializes `metadata_json.hook_decisions`, skips the handler, publishes
   failure `tool_error` / `tool_after`, and returns
   `Error::permission_denied` with `reason=blocked_by_hook`.
2. **`tool_before` rewrite.** A rewrite sink that returns
   `HookDecision{ .kind=rewrite, .rewritten_input_json=... }` causes the
   permission engine to evaluate against the rewritten input, the
   handler to receive the rewritten input, the `AuditEvent`'s
   `context` map to carry both `input_hash` and
   `rewritten_input_hash`, and the `outcome` to be `rewritten`
   (followed by the verdict on the rewritten input). The agent
   transcript records the rewritten input, never the original.
   **Status (slice 91):** shipped for direct dispatch. Rewrites replace
   the effective input before workspace resolution, permission evaluation,
   broker checks, audit, handler execution, and advisory follow-up
   payloads. The audit row records the rewritten input hash as the row's
   `input_hash` plus `metadata_json.original_input_hash` and
   `metadata_json.rewritten_input_hash`, and allowed rewritten calls use
   `AuditOutcome::rewritten`. Malformed rewrite decisions that omit
   `rewritten_input_json` are treated as `blocked_by_hook`.
3. **`tool_before` require_approval.** A sink that returns
   `HookDecision{ .kind=require_approval }` causes
   `Registry::dispatch` to consult the broker exactly as if a
   `Verdict::ask` rule had matched, even when the permission engine
   would otherwise have returned `allow`. Approval succeeds →
   `outcome=approved`; approval fails → `outcome=rejected`.
   **Status (slice 91):** shipped for direct dispatch. A hook
   `require_approval` decision promotes an otherwise-allow permission
   decision into the existing broker path; approved calls run with
   `outcome=approved`, rejected tokens keep `outcome=rejected`, and an
   underlying permission deny is not downgraded.
4. **`permission_ask_rendered` round-trip.** When a `Verdict::ask`
   rule matches, the dispatch pipeline publishes
   `permission_ask_rendered` *blocking*; the sink's `HookDecision`
   carries the operator's response identity in `reason` (e.g.
   `reason=operator_approved:huxint`); the broker accepts the
   resulting `ApprovalToken` and resumes dispatch. A
   `kind=veto` decision aborts with `outcome=rejected,
   reason=operator_denied`. **Status (slice 94):** shipped for direct
   dispatch. `Registry::dispatch` publishes a typed
   `PermissionAskRenderedPayload`, stores every consulted ask-sink decision
   under `metadata_json.permission_ask_decisions[]`, issues and immediately
   checks a broker token on `proceed`, optionally copies it to
   `DispatchContext::approval_token_output`, and preserves the legacy
   `approval_required` path when no ask sink is subscribed. Slice 95 adds the
   concrete `oran-cli` operator prompt that renders the payload and returns
   the expected operator approval/denial reason strings through the blocking
   trace. **Status (slice 96):** the fake-provider loop now covers the same
   bridge at the agent boundary. `agent::Loop` supplies a fresh
   `DispatchContext::now` for the direct dispatch, the prompt payload's
   `requested_at` is greater than the default epoch, the caller's context value
   is restored after dispatch, and the issued token replays against that prompt
   time.
5. **Sink ordering**. Three blocking sinks subscribed to the same
   event execute in subscription order; the first non-`proceed`
   short-circuits the rest. All three decisions are recorded in
   `AuditEvent::context.hook_decisions` (a JSON array of
   `{sink_id, kind, reason}`). Pinned by a multi-sink test.
   **Status (slice 91):** shipped for direct dispatch. The bus now
   returns `HookDecision::trace`, and `Registry::dispatch` serializes it
   to `metadata_json.hook_decisions`. Since v1 short-circuits at the
   first non-proceed, the array length is bounded by the index of the
   first decisive sink and contains every consulted sink.
6. **Timeout.** A sink whose handler awaits longer than
   `config.hooks.timeout_ms` causes the dispatch to abort with
   `outcome=blocked_by_hook, reason=hook_timeout`; the
   `AuditEvent::context` records the offending `sink_id` and
   `elapsed_ms`. Tested against a controllable-latency fake sink.
   **Status (slice 92):** shipped for direct dispatch. `oran-config`
   parses `hooks.timeout_ms` as a positive integer (default 2000), the
   checked-in example config carries that default, `bootstrap::run`
   threads the value into `RuntimeAssemblyOptions::hook_blocking_timeout`,
   and the assembly-owned `hook::Bus` applies it per blocking sink.
   `Bus::publish_blocking` returns a synthesized veto with
   `reason=hook_timeout`, and `Registry::dispatch` records
   `AuditOutcome::blocked_by_hook` plus
   `metadata_json.hook_decisions[].elapsed_ms` before skipping the
   handler.
7. **Sink error.** A blocking sink returning `Error::internal` is
   treated as veto with `reason=hook_error`; the underlying error
   message rides in `AuditEvent::context.error`.
   **Status (slice 91):** shipped through dispatch. A sink returning
   `core::Error` or throwing surfaces as `HookDecision{ veto,
   reason="hook_error: <message> [sink=<id>]" }`; direct dispatch records
   `outcome=blocked_by_hook` and writes the reason inside
   `metadata_json.hook_decisions`. Slice 93's direct-dispatch
   `hook_publish` row also mirrors hook-error reasons into
   `metadata_json.error` for the joined trace/audit view.
8. **Advisory unchanged.** Existing advisory publishes
   (`tool_after`, `iteration_end`, etc.) maintain their
   `publish_advisory` contract — sinks observe, no decision is
   honoured, no dispatch flow change. Regression-test asserts that
   `publish_advisory` bytes-on-the-wire equal pre-spec.
   **Status (slice 91):** shipped. `publish_advisory` remains advisory
   for advisory events; `tool_before` moved to the blocking path inside
   `Registry::dispatch`, while `tool_dispatched` / `tool_error` /
   `tool_after` keep the previous advisory contract.
9. **Type safety.** `publish_blocking<Event::tool_after>` fails to
   compile (no `EventTraits<tool_after>::Decision`). Pinned by a
   compile-fail test under the existing `tests/compile-fail` harness
   pattern.
   **Status (slice 91):** shipped. The `HasBlockingDecision<E>` concept
   is the publish-blocking constraint;
   `tests/hook/test_publish_blocking.cpp` pins the whitelist with
   `STATIC_REQUIRE`/`STATIC_REQUIRE_FALSE` over the v1 events plus
   `tool_after` / `iteration_start` / `memory_read_before` /
   `permission_denied` as negative cases. A dedicated
   `tests/compile-fail` harness does not yet exist in this repo;
   when it lands, a dedicated compile-fail file can replace the
   `STATIC_REQUIRE_FALSE` cases.
10. **`tests/hook/`** ≥ 90% coverage of the matrix (event × decision
    kind × sink ordering × timeout × error).
    **Status (slice 92):** decision-kind, ordering, error, exception,
    timeout, and dispatch-consumer axes are covered. The dispatch-side
    timeout regression uses a controllable-latency sink and asserts the
    `blocked_by_hook` audit row plus `elapsed_ms`.
11. **`bench/oran-hook/publish_blocking_overhead`** reports a
    single-sink blocking publish cost ≤ 2× the single-sink advisory
    publish baseline (~446 ns per `bench-hook/publish_one_sink`), and
    a no-sink blocking publish ≤ 1.5× the no-sink advisory baseline.
    **Status (slice 91):** shipped as additional `bench-hook`
    scenarios: `publish_blocking_no_sinks`, `publish_blocking_one_sink`,
    `publish_blocking_three_sinks_all_proceed`, and
    `publish_blocking_short_circuit_second`.

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
  — the scheduler owns future batched dispatch, cancellation slots, and
  tool per-call timeout enforcement; direct `Registry::dispatch` is now
  the first `tool_before` blocking consumer and inherits the
  bus-level blocking-hook timeout from `hook::BusOptions`.
- [`0014-structured-tool-output.md`](0014-structured-tool-output.md) —
  `HookDecision::rewritten_input_json` uses the same serialized JSON byte
  envelope as `Output::data_json` to keep the public header compile-cheap.
- [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md)
  "Permission Ordering" — the canonical order moves to the seven-step
  list above; the doc-side edit lands in the same slice as v1.

## Risks

- **Wrong consumer leads to wrong contract.** If the operator-prompt
  sink had been the *only* blocking consumer for too long, the contract
  could have baked in render-specific assumptions. Mitigation already
  shipped: slices 90-94 landed the generic bus, `tool_before` rewrite /
  veto / require_approval consumers, timeout policy, hook-publish audit rows,
  and the dispatch-side ask bridge before slice 95 added the terminal sink.
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
xmake run test-hook                    # decision matrix + ordering + timeout + error
xmake run test-tool                    # dispatch order and timeout with blocking hook
xmake build bench-hook
xmake run bench-hook
xmake run orangutan -- --explain-rules  # rule-side unchanged; hook bindings surface
```

## Out-of-Band Cross-Cuts

- `docs/design-docs/permissions-and-hooks.md` "Synchronous vs. Async
  Hooks" gains the `publish_blocking` signature and the v1 whitelist;
  the design-doc edit lands in the same slice as v1.
- `docs/design-docs/tool-runtime.md` "Permission Ordering" updates to
  the seven-step canonical order.
- `docs/exec-plans/tech-debt-tracker.md` — slice 95 removes the
  2026-05-18 hook row because the concrete operator-prompt sink has shipped.
- `docs/SECURITY.md` — gains a "Hook-driven veto" subsection citing
  spec 0015 once v1 ships, so the workspace-confinement claim (spec
  0013) and the hook-veto claim sit side-by-side.

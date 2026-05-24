## [2026-05-24 17:00] | Task: `hook::Bus::publish_blocking<E>` foundation for spec 0015 v1

### Execution Context

- Agent: `Claude`
- Base model: `Opus 4.7`
- Runtime: `orangutan-refactor` local workspace
- Linked plan: none — focused continuation of the spec-dependency-graph item
  (`0013 → 0011 + 0012 → 0014 → 0016 → 0017 → 0015 → 0018`) that unblocks
  spec 0018 AC5 (hook publish rows) once the dispatch consumer lands.

### User Query

> Continue deeply understanding the project architecture and implementation
> progress, then keep advancing the project code implementation.

### Changes Overview

- Areas: `oran-hook`, spec-0015 acceptance criteria, docs/history/version.
- Key actions:
  - Opened `<oran/hook/decision.hpp>` with the new `HookDecisionKind` enum
    (`proceed`, `veto`, `rewrite`, `require_approval`) and the
    `HookDecision` value type carrying `kind`, free-form `reason`,
    optional `rewritten_input_json` (serialised JSON bytes — same envelope
    as `ToolBeforePayload::input_json`, no `nlohmann` dependency in the
    public header per critical rule C6), and optional
    `approval_expires_at` (only meaningful for `require_approval`). The
    `rewritten_input_json` shape resolves the spec's
    `std::optional<nlohmann::json_fwd>` placeholder by reusing the same
    serialised-bytes envelope `tool::Output::data_json` and
    `ToolBeforePayload::input_json` already use; that keeps the public
    header free of `<nlohmann/json.hpp>` without forcing a pimpl, and the
    bytes pass straight through audit / permission re-evaluation without
    re-parse / re-serialise round trips.
  - Opened `<oran/hook/event_traits.hpp>` with an empty primary
    `EventTraits<E>` template plus explicit specialisations for the
    spec-0015 v1 whitelist (`Event::tool_before`,
    `Event::permission_ask_rendered`, `Event::memory_write_before`) that
    set `Decision = HookDecision`. The `HasBlockingDecision<E>` concept
    over `EventTraits<E>::Decision` gates `publish_blocking<E>`; every
    other event leaves the trait empty so `publish_blocking<Event::tool_after>`
    fails the `requires` clause at the call site (the v1 acceptance
    criterion #9 dependency).
  - Extended `hook::Sink` with a new
    `handle_blocking(Event, Payload) -> Awaitable<Result<HookDecision>>`
    virtual whose default body lives in the new `src/oran-hook/sink.cpp`
    and returns `HookDecision{}` (i.e. `proceed`). Sinks that only care
    about advisory events do not have to override it — every existing
    `Sink` implementation in the codebase continues to compile unchanged.
    The coroutine body stays out of the public header to keep per-TU
    compile cost bounded.
  - Extended `hook::InProcessSink` with an optional
    `BlockingCallback = std::function<Awaitable<Result<HookDecision>>(Event, Payload)>`
    setter (`set_blocking_handler`) plus an `handle_blocking` override
    that forwards to the callback when set and falls back to the
    `Sink::handle_blocking` default otherwise. The existing
    `InProcessSink(id, callback, kind)` constructor signature is
    unchanged so the in-tree sinks (slice-22 audit recording, the agent
    loop's session-promotion observer) keep their construction sites.
  - Added `hook::Bus::publish_blocking<E>(Payload) -> Awaitable<Result<HookDecision>>`
    constrained by `requires HasBlockingDecision<E>`. The template
    delegates to a private non-template `publish_blocking_impl(Event,
    Payload)` defined in `bus.cpp`, which walks subscribed sinks in
    subscription order, awaits each one's `handle_blocking`, applies the
    same per-sink redaction the advisory path uses (slice-65
    `ToolAfterPayload::data_json` policy), and short-circuits at the
    first non-`proceed` decision (spec 0015 v1 §"Sink resolution order").
    A sink that returns `std::unexpected(error)` or throws is converted
    to a veto with `reason="hook_error: <message> [sink=<id>]"` — the
    spec-mandated failure classification carrying the underlying cause
    forward for the upcoming audit row writer. With no sinks subscribed,
    or when every subscribed sink returns `proceed`, the bus yields a
    default-constructed `HookDecision{}` (kind = `proceed`, empty
    `reason`).
  - Added `tests/hook/test_publish_blocking.cpp` covering the v1
    boundaries the API ships: empty bus → proceed, single-sink each
    decision kind (`proceed`, `veto`, `rewrite` with rewritten input,
    `require_approval`), `InProcessSink::set_blocking_handler` drives
    decisions through the std::function-backed path, multi-sink
    short-circuit on first non-`proceed`, consult-next-sink when the
    earlier sink returns `proceed`, all-`proceed` returns proceed, sink
    `core::Result` error and exception both yield veto with
    `reason=hook_error: ... [sink=<id>]`, and a `STATIC_REQUIRE` matrix
    pinning the `HasBlockingDecision<E>` whitelist
    (`tool_before` / `permission_ask_rendered` / `memory_write_before`
    permitted; `tool_after` / `iteration_start` / `memory_read_before` /
    `permission_denied` rejected). The `STATIC_REQUIRE_FALSE` pins
    spec-0015 AC #9 ("type safety") without requiring a
    compile-fail harness that does not yet exist in this repo.
  - Bumped `kVersion` to `2.0.0-slice90` in `src/oran-bootstrap/bootstrap.cpp`.
  - Extended the `<oran/hook.hpp>` umbrella header with the new
    `decision.hpp` / `event_traits.hpp` includes.

### Design Intent

Spec 0018 left two named downstream items after slice 89: hook publish
rows (AC5) and binary handoff. Binary handoff is blocked on the absent
real provider adapter (multi-slice transport + protocol + retry work).
Hook publish rows are blocked on spec 0015's blocking-veto semantics —
the dispatch consumer needs to know whether a `tool_before` sink
proceeded, vetoed, rewrote, or required approval before it can record
a per-sink decision row joined to the spec-0018 `trace_turns` row.

The tech-debt tracker has carried the hook-bus row since 2026-05-18
with the explicit precondition "deferred to a follow-up slice when the
first blocking consumer needs them"; spec 0017's text-only loop landed
in slice 75, the tool-dispatch loop in slice 76, and the spec-0018
trace writer in slice 80, so the agent-loop now exists and the next
slice it consumes is exactly the blocking publish surface. The deep-
review §5.3 follow-up is also explicit: *do not build the render-side
ask flow in `oran-agent` as a special case; make it the first consumer
of `publish_blocking`.* This slice ships the publish surface without
that consumer — the dispatch consumer is the natural next slice and
the operator-prompt sink (the v1 first consumer per spec 0015) is the
slice after that.

The slice intentionally stops at the **bus surface + sink contract**
and does not yet wire `Registry::dispatch` to consume blocking
decisions. The reasons:

1. The dispatch consumer changes audit semantics (new outcomes
   `blocked_by_hook` / `rewritten`) and the canonical seven-step
   dispatch order both. Bundling those into one slice with the API
   surface blows the ~600 LoC / ~6 file guideline and obscures the
   contract test → consumer test progression.
2. Spec 0015 v1's acceptance criteria #1-3 (`tool_before` veto /
   rewrite / require_approval) all want the audit-side observable
   changes that the consumer slice owns; criterion #4 wants the
   ApprovalBroker round-trip; the API-only acceptance criterion is the
   sink-resolution-order subset (this slice's coverage) plus the
   compile-time event whitelist.
3. The slice keeps the audit-side outcomes (`AuditOutcome::blocked_by_hook`
   / `rewritten`) deferred until the same slice that consumes them. A
   half-shipped audit enum risks an unused enumerator that the next
   reviewer has to triage.

The slice does carry the **error-classification contract** (sink error /
exception → veto with `reason=hook_error`) because the bus's error path
shape is part of the bus contract regardless of consumer.

Naming note: spec 0015 sketched `std::optional<nlohmann::json_fwd>` for
`HookDecision::rewritten_input`, but `std::optional<T>` requires a
complete type at instantiation, so the public header would need to
include `<nlohmann/json.hpp>` and break critical rule C6. The shipped
shape uses `std::optional<std::string>` (`rewritten_input_json`)
matching the existing `ToolBeforePayload::input_json` and
`tool::Output::data_json` envelopes. Spec 0015 v1's status block is
updated to reflect the shipped name.

### Files Modified

- `include/oran/hook/decision.hpp` (new)
- `include/oran/hook/event_traits.hpp` (new)
- `include/oran/hook/sink.hpp`
- `include/oran/hook/in_process_sink.hpp`
- `include/oran/hook/bus.hpp`
- `include/oran/hook.hpp`
- `src/oran-hook/sink.cpp` (new)
- `src/oran-hook/in_process_sink.cpp`
- `src/oran-hook/bus.cpp`
- `tests/hook/test_publish_blocking.cpp` (new)
- `src/oran-bootstrap/bootstrap.cpp` (version bump)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 90, pointed at this history,
  refreshed `oran-hook` test counts (17 → 29 cases, 109 → 172
  assertions), updated the next-intended-slice text to call out the
  dispatch consumer + operator-prompt sink as the two follow-up
  slices, and narrowed the tech-debt row line.
- `docs/ARCHITECTURE.md` — extended the `oran-hook` library inventory
  row to mention `publish_blocking<E>`, `HookDecision`,
  `HookDecisionKind`, `EventTraits<E>`, `HasBlockingDecision`, and the
  `Sink::handle_blocking` default + `InProcessSink` blocking handler
  surface.
- `docs/QUALITY_SCORE.md` — refreshed `oran-hook` test counts.
- `docs/design-docs/permissions-and-hooks.md` — bus status block
  updated to record `publish_blocking<E>` shipping; the
  `Synchronous vs. Async Hooks` section's `EventTraits` example
  rewritten to use the shipped generic `HookDecision` (per spec 0015
  v1 rather than the design-doc's earlier per-event placeholder); the
  `Bus` skeleton gains the new method signature.
- `docs/product-specs/0015-blocking-hook-decisions.md` — status block
  added under v1 plus per-AC status pointers for AC #1-#4 (consumer-
  pending) and AC #9 (shipped: `STATIC_REQUIRE_FALSE` covers it
  through the `HasBlockingDecision` concept).
- `docs/exec-plans/tech-debt-tracker.md` — narrowed the 2026-05-18
  hook row to reflect that the publish surface + value types are
  shipped; the remaining work is `Registry::dispatch` consumption +
  new `AuditOutcome` enumerators + the first operator-prompt sink.
- `docs/releases/feature-release-notes.md` — added the slice 90
  release note.

### Validation

- Commands run:
  - `xmake build oran-hook`
  - `xmake build test-hook`
  - `xmake run test-hook` (29 cases / 172 assertions all pass)
  - `xmake build oran-tool oran-agent oran-bootstrap orangutan`
    (downstream rebuilds clean)
  - `xmake run test-tool` (166 cases / 1590 assertions; no regression)
  - `xmake run test-agent` (23 cases / 363 assertions; no regression)
  - `xmake run test-bootstrap` (56 cases / 221 assertions; no regression)
  - `xmake run test-permission` (89 cases / 414 assertions; no regression)
- Tests added/changed:
  - `publish_blocking on empty bus returns proceed`
  - `Sink default handle_blocking returns proceed`
  - `publish_blocking returns single sink's veto decision`
  - `publish_blocking returns single sink's rewrite decision`
  - `publish_blocking returns single sink's require_approval decision`
  - `publish_blocking short-circuits at first non-proceed sink`
  - `publish_blocking consults later sink when earlier returns proceed`
  - `publish_blocking returns proceed when all sinks proceed`
  - `sink error becomes veto with reason=hook_error`
  - `throwing sink becomes veto with reason=hook_error`
  - `InProcessSink blocking handler drives the decision`
  - `EventTraits encodes the v1 blocking whitelist`
- Bench impact: not measured this slice. Spec 0015 v1 AC #11 calls for
  a `bench-hook/publish_blocking_overhead` scenario ≤ 2× the
  single-sink advisory baseline (~446 ns); that lives with the
  dispatch consumer slice so the bench can exercise the
  same-shape-as-advisory short-circuit path the consumer actually
  takes.
- Compile-budget delta: not measured; the new public types are
  `nlohmann`-free, the template `publish_blocking<E>` forwards to a
  non-template `publish_blocking_impl` in `bus.cpp`, and the default
  `Sink::handle_blocking` body lives in `sink.cpp` rather than the
  header so consumers do not pay coroutine-machinery cost in every
  TU.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: the 2026-05-18 hook row now narrows to the
  dispatch consumer + `AuditOutcome` enumerators + first
  operator-prompt sink. Spec 0018 AC5 stays open until the dispatch
  consumer ships the `hook_publish` audit row writer.
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)

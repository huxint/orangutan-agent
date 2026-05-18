## [2026-05-18 11:00] | Task: land `oran-hook` foundation + `Registry::dispatch` hook-bus tap (advisory `tool_before` / `tool_after`)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — single-session slice that fits the
  `Next intended slice` bullet in
  [`STATUS.md`](../../STATUS.md) ("the hook bus scaffolding that
  `Registry::dispatch` will publish `tool_before` / `tool_after` events
  to once `oran-hook` exists"), matching `PLANS_GUIDE.md` "When NOT
  To Create A Plan".

### User Query

> 深度了解项目，查看当前项目真实进度, 继续推进项目代码的实现.
> 先commit当前的实现. ultrathink.
>
> (Understand the project deeply, check the real current progress,
> continue advancing the project code implementation. Commit the
> current implementation first. Ultrathink.)

The user re-issued the autonomous-progress prompt and explicitly asked
to commit the pending slice 21 (approval-broker wiring) first and then
push forward. After committing slice 21 (one-shot `feat(tool):` commit
referencing the slice-21 history file), the three remaining
`Next intended slice` candidates were: the first provider adapter
(Anthropic Messages), signal-aware shutdown for `bootstrap::run`, and
the hook bus scaffolding. The provider adapter is genuinely
multi-slice (transport + protocol + execution + integration tests) so
it needs an exec plan before the first slice lands; signal-aware
shutdown is low-value mid-construction (no agent loop yet to interrupt);
the hook bus scaffolding is the natural follow-up to slice 21 because
the `Registry::dispatch` integration is the design-doc consumer the
bus has been waiting on. Slice 22 picks the hook bus + dispatch wiring
so a sink can observe every tool invocation from this slice forward.

### Changes Overview

- **New library `oran-hook`.** Five public headers + one facade + two
  implementation files:
  - `include/oran/hook/event.hpp` — `Event` enum covering 38
    lifecycle events (agent / provider / tool / memory / channel /
    orchestration / automation / session / permission); `Mode` enum
    (`advisory` / `blocking`) + `default_mode(Event)` returning
    `blocking` for the three design-doc pre-action gates (`tool_before`,
    `memory_*_before`, `permission_ask_rendered`) and `advisory`
    otherwise.
  - `include/oran/hook/payload.hpp` — `Identity` struct (scope_key,
    agent_key, identity strings every event payload duplicates);
    `ToolBeforePayload { tool_name, input_json, who, started_at }`;
    `ToolAfterPayload { tool_name, input_json, who, succeeded,
    output_text, error_kind, error_message, started_at, finished_at,
    duration }`; `Payload = std::variant<std::monostate,
    ToolBeforePayload, ToolAfterPayload>`. Events without typed
    shapes carry `std::monostate` for now; the typed shape lands with
    the producing subsystem.
  - `include/oran/hook/sink.hpp` — abstract `Sink` interface
    (`id() -> string_view`, `receive(Event, Payload) ->
    Awaitable<Result<void>>`). Non-copyable, non-movable (the bus
    keeps raw pointers).
  - `include/oran/hook/bus.hpp` — `PublishOutcome { sinks, failure_count,
    all_succeeded }` and `Bus` (move-only, `bind(Sink&, span<Event>)` /
    `bind(Sink&, initializer_list)` / `unbind(Sink&)` /
    `publish_advisory(Event, Payload) -> Awaitable<PublishOutcome>` /
    `binding_count()` / `sink_count(Event)`).
  - `include/oran/hook/in_process_sink.hpp` — `InProcessSink` final
    class taking `(string id, Callback callback)`; `Callback` is the
    typedef for `std::function<Awaitable<Result<void>>(Event, Payload)>`.
    Empty callback returns `invalid_argument` on receive (defensive
    against misuse).
  - `include/oran/hook.hpp` — umbrella header.
  - `src/oran-hook/bus.cpp` — `PublishOutcome::all_succeeded` +
    `failure_count` via `std::ranges::*`; `Bus::bind` deduplicates
    `(sink, event)` pairs so re-binding the same pair is a no-op;
    `Bus::unbind` iterates every event vector and removes the sink
    pointer via `std::erase`; `Bus::publish_advisory` looks up the
    event's vector, iterates in subscription order, awaits each
    sink's `receive`, and records the per-sink outcome — never
    aborting on a sink error (advisory contract).
  - `src/oran-hook/in_process_sink.cpp` — `receive` checks the
    callback is non-empty (returns `invalid_argument` with the sink
    id attached if empty), otherwise forwards verbatim.
- **xmake wiring.** `xmake/targets.lua` adds
  `oran_lib("hook", { "oran-core", "oran-async" }, {})` immediately
  after `oran-permission`, grows `oran-tool`'s deps to include
  `oran-hook`, and adds `oran-hook` to the `orangutan` binary's
  `add_deps` list (after `oran-permission`, before `oran-tool`).
  `xmake/tests.lua` adds `oran_test("hook", { "oran-hook" })` after
  `test-permission`. `xmake/bench.lua` adds `oran_bench("hook",
  { "oran-hook" })` and grows `bench-tool`'s deps to include
  `oran-hook` (the new `bench/tool/scenarios/hooks.cpp` needs to
  construct a `hook::Bus`).
- **`DispatchContext` extension.** One new field:
  - `hook::Bus* bus{nullptr}` — non-owning pointer the agent loop
    sets to publish lifecycle events. When `null`, dispatch
    reproduces the slice-21 behavior exactly (no publish, no audit
    delta). When non-null, dispatch publishes `tool_before` after
    the registry resolves the tool def and `tool_after` at every
    exit (handler success, permission deny, broker rejection, audit
    error). The header now also includes `<oran/hook/bus.hpp>` —
    forward-declaration is not enough because the optional field
    needs the full type so the `make_ctx` brace-init pattern keeps
    working (pointer to incomplete is a designated-init landmine).
- **`Registry::dispatch` restructure.** The function was a tower of
  early `co_return std::unexpected(...)` branches; slice 22
  collapses that into a single `result` variable rebound by every
  branch and a single final `co_return result`. The flow is:
  1. Lookup tool. Unknown name → `co_return Error::not_found`
     **before** any hook publish — the dispatch never started, so
     sinks see nothing.
  2. `started_at = core::time::now_utc()`.
  3. If `ctx.bus != nullptr`, publish `tool_before` (the publish
     outcome is `[[maybe_unused]]` — sinks observe; their errors
     don't change the dispatch).
  4. Inner scope: rule evaluation + broker check + audit record +
     verdict switch all set the `result` variable instead of
     returning directly. The `audit.record` failure case sets
     `result` to the storage error and skips the switch; otherwise
     the switch's three branches (`deny` / `ask` / `allow`) each
     assign to `result`.
  5. If `ctx.bus != nullptr`, publish `tool_after` with
     `ToolAfterPayload` populated from `result` (`succeeded` from
     `result.has_value()`; `output_text` from `result->text` on
     success; `error_kind` from `core::enum_name(result.error().kind())`
     on failure; `error_message` from `result.error().message()` on
     failure; `started_at` from step 2; `finished_at` from a fresh
     `core::time::now_utc()`; `duration` from the chrono diff
     duration-cast to `nanoseconds`).
  6. `co_return result`.
  The unreachable "fell off the bottom" case initialises `result`
  with `Error::internal("dispatch did not produce a result")` so
  static analysis sees no uninitialised path; the switch covers
  every verdict so the placeholder never actually escapes.
- **Tests.** `tests/hook/test_bus.cpp` ships ten cases covering
  empty bus + default state, empty-bus advisory publish, one-sink
  bind + receive + outcome, multi-event bind (one sink, two events),
  multiple sinks in subscription order, idempotent bind (duplicate
  pair de-duplicated, partial-duplicate adds only the new event),
  full sink unbind (count of removed bindings, dispatched events
  reach the kept sink only), unbind of unsubscribed sink returns
  zero, sink errors captured in outcome but other sinks still run,
  and `default_mode` blocking-for-pre-action / advisory-otherwise.
  `tests/hook/test_in_process_sink.cpp` ships four cases (`id()`
  reports construction id, `receive` forwards event + payload to
  the callback, callback errors propagate verbatim, empty callback
  returns `invalid_argument`). `tests/tool/test_registry.cpp` grows
  to 58 cases / 477 assertions (+8 cases / +63 assertions) covering
  the eight new dispatch-with-hook paths: allow + `tool_before` +
  `tool_after`, deny + `tool_after` with `error_kind=permission_denied`,
  handler-internal-error + `tool_after` with the handler's error
  kind, unknown-tool silent-skip (no hook publish), failing-sink
  advisory swallow (dispatch result is preserved), null-bus
  backwards-compat (no hook publish, audit row still recorded),
  ask short-circuit + `tool_after` with `error_kind=permission_denied`
  + `reason=approval_required`, ask + broker-reject + `tool_after`
  with `error_kind=permission_denied` and `reason=replay_exhausted`
  in the dispatch error.
- **Bench.** New `bench/hook/scenarios/bus.cpp` ships a three-way
  A/B/C — `publish_no_sinks` (~242 ns, empty bus map lookup +
  early return; unstable in nanobench's eyes because the scenario
  is sub-microsecond but the number is informative), `publish_one_sink`
  (~446 ns, ~204 ns over the empty-bus baseline — the per-first-sink
  cost is the bus's `std::find` deduplication walk on bind + the
  await of the sink's coroutine on publish), `publish_three_sinks`
  (~698 ns, ~126 ns per additional sink — confirms linear scaling).
  New `bench/tool/scenarios/hooks.cpp` ships a three-way A/B/C
  contrast of the allow path — `dispatch_allow_no_hooks` (~2.1 µs,
  the slice-17 baseline), `dispatch_allow_with_empty_bus` (~2.4 µs,
  ~346 ns "bus attached, nothing listens" tax), `dispatch_allow_with_two_sinks`
  (~3.0 µs, ~914 ns "bus attached with two observers" tax). Both
  deltas are small relative to the audit-record cost (~18 µs
  StorageAuditSink) — the hook bus is not on the agent loop's
  critical path.
- **Slice-version bump.** `kVersion` 21 → 22. `xmake run orangutan --help`
  reports `orangutan v2.0.0-slice22`.

### Design Intent

**Why the hook bus is the right closing slice for the pre-agent-loop
infrastructure.** The permission module is done (slices 7–21). The
tool registry is feature-complete for what the agent loop needs (slice
21 closed the `Verdict::ask` mediation). The remaining "infrastructure
without a consumer" piece was the hook bus — `permissions-and-hooks.md`
enumerates 38 lifecycle events but the only thing that existed for
them was the design doc. The `Registry::dispatch` integration is the
first consumer the bus needs to justify its existence; wiring it now
means the next slice (whichever produces the first event the bus
publishes from outside `oran-tool`) inherits a working publish/subscribe
surface.

**Why advisory-only in this slice.** The design contemplates blocking
hooks where a sink can veto / rewrite the action — `tool_before` is
the canonical example ("rewrite input or short-circuit"). But the
first blocking consumer is the operator-prompt sink that will render
the `permission_ask_rendered` event; that sink lives in `oran-agent`,
which doesn't exist yet. Adding the `publish_blocking` overload now
would land code with no caller. Slice 22 ships the advisory branch
the dispatch integration needs; `publish_blocking` lands when the
first blocking sink is queued up (tracked in `tech-debt-tracker.md`).

**Why `tool_before` + `tool_after` only, not all four tool events.**
The bookend pair covers every reasonable observability use case (audit
replay, latency tracking, "what tool was called and how did it end").
`tool_dispatched` (post-permission, pre-handler) is most useful when
the agent loop wants a hook between permission resolution and handler
entry — adding it now ahead of a consumer would inflate the dispatch
function for unused publish points. `tool_error` is informative when
a sink wants the failure-only narrow path; today, `tool_after` carries
the failure information in `error_kind` + `error_message`, so the
failure-only sink can filter on `succeeded == false`. Both are tracked
in `tech-debt-tracker.md` and land when the agent loop has a use for
them.

**Why advisory hooks return `PublishOutcome` instead of just
`Awaitable<void>`.** The agent loop's likely policy when a sink fails
is "log it and continue" — the bus could swallow errors silently and
just log them, but that hides observability gaps from the caller.
`PublishOutcome` lets the caller surface sink failures into its own
audit log (or `--explain-hooks` flag, future) without coupling the bus
to a single error policy. The outcome is also useful for tests: the
slice-22 test suite asserts the per-sink outcome explicitly rather
than scraping stderr for sink errors. The `[[maybe_unused]]` annotation
on the dispatch callsite signals that *for dispatch's purposes*, the
outcome is discardable — the caller doesn't escalate sink errors to
dispatch errors. A future "audit hook errors" sink wraps the bus in
a decorator that escalates.

**Why the bus stores `Sink*` rather than `std::shared_ptr<Sink>`.**
The bootstrap layer owns sink lifetimes for the duration of the
process; the bus is a transient publisher. Reference semantics are
natural here — copies would imply ownership transfer, which is wrong
for an observer pattern. The non-copyable / non-movable contract on
`Sink` matches `permission::AuditSink`, which has the same design
constraint (virtual base + reference identity).

**Why `started_at` and `finished_at` are computed inside dispatch
rather than supplied by the caller.** The caller's `ctx.now` is the
approval broker's clock — pinned per-turn for deterministic broker
behavior. Hook timing wants real wall-clock so a sink can compute
duration; reusing `ctx.now` would make every dispatch report a
duration of zero. The cost is two `core::time::now_utc()` calls per
dispatch (~2 ns each on the bench host) which is below the noise
floor of the dispatch itself.

**Why `tool_before` fires after lookup, not before.** The design doc
says `tool.before(name, input, identity)` — it doesn't require the
tool to be known. But practically, firing for unknown tools spams the
sink with garbage. Sinks subscribed to `tool_before` care about real
tools the registry has a definition for; the unknown-name error is
its own thing (the agent loop catches it before the LLM ever sees a
result). Slice 22 picks the "fire only for known tools" semantics; a
future slice can add a `tool_lookup_failed` event if the observability
case ever appears.

**Why the `Result<Output>` placeholder in dispatch is
`Error::internal("dispatch did not produce a result")` rather than
`Error::cancelled` or some sentinel.** The placeholder catches a
genuine bug (a future verdict added to the enum that the switch does
not handle); `internal` carries the "this should never happen" semantics
correctly. The runtime never sees this string in normal operation —
the switch covers every verdict, the audit-record failure path
overwrites the placeholder, and so on.

**Why bench `dispatch_allow_with_empty_bus` is a separate scenario
from `dispatch_allow_with_two_sinks`.** Operators may wire the bus
into bootstrap unconditionally (it's cheap) but only bind sinks per
config. The "wired but no sinks" cost is the cost of that
unconditional wiring — measurable separately so a future "lazy bus
construction" optimisation has a concrete number to beat. The
(with_two_sinks − no_hooks) delta is the actual per-call observability
tax; (with_empty_bus − no_hooks) is the "we left the bus attached"
tax. Both should be small relative to the audit cost; the slice-22
numbers confirm they are.

### Files Modified

- `include/oran/hook/event.hpp` — new, 87 lines, `Event` + `Mode` +
  `default_mode`.
- `include/oran/hook/payload.hpp` — new, 65 lines, `Identity` +
  `ToolBeforePayload` + `ToolAfterPayload` + `Payload` variant.
- `include/oran/hook/sink.hpp` — new, 47 lines, abstract `Sink` with
  `id` + `receive`.
- `include/oran/hook/bus.hpp` — new, 92 lines, `PublishOutcome` +
  `Bus`.
- `include/oran/hook/in_process_sink.hpp` — new, 39 lines,
  `InProcessSink` final class.
- `include/oran/hook.hpp` — new, umbrella header.
- `src/oran-hook/bus.cpp` — new, 76 lines, bus implementation.
- `src/oran-hook/in_process_sink.cpp` — new, 22 lines, sink
  implementation.
- `include/oran/tool/registry.hpp` — file header rewritten to cover
  the five-bullet composition (slice 22 hook-bus tap added); new
  include for `<oran/hook/bus.hpp>`; new `bus` field on
  `DispatchContext` with docstring; `dispatch` contract docstring
  rewritten to list the seven-step flow.
- `src/oran-tool/registry.cpp` — `dispatch` rewritten as a single
  `result` variable + final `co_return result` so the `tool_after`
  publish sees every exit path; new helpers `make_hook_identity`,
  `build_before_payload`, `build_after_payload`; includes now pull
  `<chrono>`, `<oran/core/enum_names.hpp>`, `<oran/core/time.hpp>`,
  `<oran/hook/bus.hpp>`, `<oran/hook/event.hpp>`,
  `<oran/hook/payload.hpp>`.
- `tests/hook/test_bus.cpp` — new, 10 cases / 56 assertions.
- `tests/hook/test_in_process_sink.cpp` — new, 4 cases / 23
  assertions.
- `tests/tool/test_registry.cpp` — +8 cases / +63 assertions for the
  slice-22 dispatch-with-hook paths; new `CaptureSink`/`FailingHookSink`
  helpers + the `allow_rule_set`/`deny_rule_set`/`noop_ok_handler`/
  `noop_error_handler`/`make_hooked_ctx` plumbing.
- `bench/hook/main.cpp` + `bench/hook/scenarios/bus.cpp` +
  `bench/hook/README.md` — new bench bucket.
- `bench/tool/main.cpp` — registers the new `register_tool_hooks`
  block after the existing five.
- `bench/tool/scenarios/hooks.cpp` — new, three-way A/B/C of the
  dispatch hook overhead.
- `bench/tool/README.md` — documents the new three-way scenario.
- `xmake/targets.lua` — `oran_lib("hook", ...)`, `oran-tool` dep
  graph update, `orangutan` binary deps update.
- `xmake/tests.lua` — `oran_test("hook", ...)`.
- `xmake/bench.lua` — `oran_bench("hook", ...)` + `bench-tool` dep
  graph update.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 21 → 22.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 22, history pointer, library surfaces row
  for `oran-hook` (14 cases / 79 assertions) + `oran-tool` (58 cases /
  477 assertions), and a refreshed `Next intended slice` bullet
  pointing at the Anthropic adapter + signal-aware shutdown + the
  remaining hook lifecycle (`tool_dispatched` / `tool_error`) +
  blocking-veto semantics.
- `docs/QUALITY_SCORE.md` — Hooks row rewritten to describe the new
  library surface + dispatch wiring + bench numbers; Tool registry row
  rewritten to include the slice-22 hook tap; Test framework row
  refreshed with `oran-hook` 14/79 and `oran-tool` 58/477; Bench
  harness row extended with the new `bench-hook` block + the
  `dispatch_allow_no_hooks` / `dispatch_allow_with_empty_bus` /
  `dispatch_allow_with_two_sinks` trio.
- `docs/ARCHITECTURE.md` — slice-status preamble now lists slice 22
  alongside 17/18/19/20/21; `oran-tool` inventory row updated to
  describe the hook tap; `oran-hook` inventory row rewritten to
  describe the foundation surface; the slice-status paragraph in the
  library inventory describes the slice-22 hook wiring.
- `docs/design-docs/tool-runtime.md` — Status box updated to describe
  the new `DispatchContext::bus` field and the `tool_before` /
  `tool_after` publish.
- `docs/design-docs/permissions-and-hooks.md` — new "Bus status
  (2026-05-18, slice 22)" paragraph at the top of "Hook Bus" describing
  the foundation library + dispatch wiring; the rest of the existing
  "Hook Bus" surface section stays as-is (it documents the target
  shape; what landed this slice is a strict subset that the paragraph
  enumerates).
- `docs/releases/feature-release-notes.md` — new top row
  `oran-hook-foundation-and-dispatch-wiring`.
- `docs/exec-plans/tech-debt-tracker.md` — two new top rows: advisory-
  only dispatch semantics (blocking + veto deferred); `tool_dispatched`
  + `tool_error` events enumerated but not yet published.
- `bench/tool/README.md` — documents the new three-way scenario.
- `bench/hook/README.md` — new file documenting the bench bucket.
- `docs/histories/2026-05/20260518-1100-oran-hook-foundation-and-dispatch-wiring.md` —
  this file.

### Validation

- Commands run:
  - `xmake build oran-hook` — clean (3 TUs, ~6 s).
  - `xmake build oran-tool` — clean (~13 s).
  - `xmake build test-hook` — clean (~12 s).
  - `xmake build test-tool` — clean (~38 s).
  - `xmake run test-hook` — 14 cases / 79 assertions, all green.
  - `xmake run test-tool` — 58 cases / 477 assertions, all green.
  - `xmake test` — all 10 buckets green
    (test-async / cli / config / core / hook / io / bootstrap / tool /
    permission / storage).
  - `xmake build bench-hook && xmake run bench-hook` — clean;
    measured `publish_no_sinks ~242 ns` (unstable per nanobench's
    threshold), `publish_one_sink ~446 ns`, `publish_three_sinks
    ~698 ns`.
  - `xmake build bench-tool && xmake run bench-tool` — clean;
    measured `registry.lookup ~9.21 ns`,
    `registry.dispatch_allow ~2,636 ns`,
    `file_write.dispatch_truncate ~13,068 ns`,
    `file_write.dispatch_append ~12,485 ns` (unstable),
    `file_edit.dispatch_unique_replace ~16,035 ns`,
    `file_edit.dispatch_replace_all_many ~16,586 ns`,
    `file_search.single_file_one_match ~9,240 ns`,
    `file_search.recursive_dir_many_matches ~27,726 ns`,
    `dispatch_ask_short_circuit ~2,550 ns`,
    `dispatch_ask_approved ~13,657 ns`,
    `dispatch_ask_rejected ~14,314 ns`,
    `dispatch_allow_no_hooks ~2,071 ns`,
    `dispatch_allow_with_empty_bus ~2,417 ns` (unstable),
    `dispatch_allow_with_two_sinks ~2,986 ns`.
  - `xmake build orangutan && xmake run orangutan -- --help` —
    prints the slice-22 banner; the CLI surface is unchanged.
- Tests added/changed: 14 new hook-bucket cases (+79 assertions); 8
  new tool-bucket cases (+63 assertions); no existing test required
  modification beyond import additions for the new hook types.
- Bench impact: existing scenarios unchanged within noise. New
  scenarios baselined above.
- Compile-budget delta: one new library (`oran-hook` with 2 TUs); one
  new TU in `bench-tool` (`hooks.cpp`); one new TU in `bench-hook`
  (`bus.cpp`). All consume only stdlib + asio + PCH, so build-time
  impact is in the same envelope as the previous bench scenarios.

### Follow-ups

- Issues to file: none.
- Tech-debt entries filed:
  - 2026-05-18 — Hook bus dispatch is advisory-only; blocking
    semantics with veto for `tool_before` / `memory_*_before` /
    `permission_ask_rendered` are deferred.
  - 2026-05-18 — `tool_dispatched` and `tool_error` events are
    enumerated but not yet published by `Registry::dispatch` (only
    the `tool_before` / `tool_after` bookend pair is wired).
- Linked release note: 2026-05-18
  `oran-hook-foundation-and-dispatch-wiring` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: when the agent loop lands, the
  natural integration is:
  1. The loop owns a `hook::Bus&` borrowed from the bootstrap
     assembly (a future `bootstrap::RuntimeAssembly::hook_bus()`
     accessor adds the bus alongside the existing broker + audit
     sink getters).
  2. Per turn, the loop builds *one* `DispatchContext` whose
     `bus` is set to the assembly's bus.
  3. Operators wire sinks via `config.hooks.sinks` + `config.hooks.bindings`
     (currently planned, `oran-config` doesn't yet read these).
     The bootstrap layer constructs each configured sink, binds it
     to the configured events, and the agent loop just publishes —
     no per-agent configuration.
  4. The `permission_ask_rendered` blocking flow (operator-prompt
     sink) is the first non-trivial sink; it needs
     `publish_blocking` + `EventTraits<E>::Decision` which slice 23
     or 24 ships.

## [2026-05-20 19:00] | Task: `tool_dispatched` + `tool_error` publish (slice 25)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — single-session slice that fits the tech-debt
  entry's "~30 LoC + tests" envelope and matches `PLANS_GUIDE.md`
  "When NOT To Create A Plan". The two other candidates listed in
  `STATUS.md` after slice 24 remained blocked: the Anthropic Messages
  adapter is multi-slice and needs an exec plan ahead of code; blocking
  hook semantics with veto has its first consumer in the not-yet-existing
  `oran-agent`. Completing the tool-lifecycle event taxonomy (the
  remaining two of the four events the design enumerates) is the only
  tracked tech-debt that shipped today without waiting on `oran-agent`,
  and it makes the hook surface internally consistent — the four
  enumerators in `hook::Event` now all have a publisher.

### User Query

> 深度项目架构，了解当前项目实现进度, 继续推进项目代码实现.
> 实现完成需要 commit 和 push. ultrathink.
>
> (Deep project architecture, understand current project implementation
> progress, continue advancing the project code implementation.
> Implementation completion requires commit and push. Ultrathink.)

### Changes Overview

- **New typed payloads on `<oran/hook/payload.hpp>`.** `ToolDispatchedPayload`
  carries `(tool_name, input_json, who, started_at, verdict)` — the
  `verdict` field stores the matched rule's `permission::Verdict`
  enum wire spelling (`allow` or `ask`) so a sink subscribed only to
  `tool_dispatched` can distinguish a free allow from an approved ask
  without subscribing to the audit pipeline. `ToolErrorPayload` is the
  failure-only narrow channel: same identity columns as `tool_after`
  plus `error_kind` + `error_message`, but no `succeeded` / `output_text`
  (the variant alternative already conveys "this is a failure"). Both
  types join the `Payload` variant; the variant grew from three to
  five alternatives.
- **`Registry::dispatch` publishes the two new events.** After the
  audit row records successfully, the dispatcher computes
  `handler_about_to_run = (verdict == allow) || (verdict == ask &&
  broker_consulted && !broker_rejection)`; when true and the bus is
  non-null, it publishes `tool_dispatched` with the
  resolved verdict. After the dispatch result is bound and just before
  the existing `tool_after` publish, when `!result` and the bus is
  non-null, it publishes `tool_error` carrying the propagated error
  kind/message. Both publishes share `started_at` (and `tool_error`
  shares `finished_at` with the same-tick `tool_after`) so a sink can
  correlate the four events of a single call via the timestamp pair.
  All four events stay advisory — sink errors land in the
  `PublishOutcome` but do not change the dispatch result.
- **Slice-version bump.** `kVersion` 24 → 25. `xmake run orangutan
  --help` reports `orangutan v2.0.0-slice25`.
- **Tests.** `tests/tool/test_registry.cpp` grows by eleven
  `[unit][tool][hook]` cases that cover every branch the new
  publishes need to honour:
  - `tool_dispatched` on the allow path with `verdict == "allow"`.
  - `tool_dispatched` suppressed on the deny path.
  - `tool_dispatched` suppressed on the ask short-circuit path
    (broker/token not supplied — slice-17/18/19/20 behaviour).
  - `tool_dispatched` on the ask-approved path with
    `verdict == "ask"` — the rule's verdict, not the audit row's
    promoted outcome.
  - `tool_dispatched` suppressed on broker rejection
    (`replay_max=0` exhausted at issuance).
  - `tool_error` on handler failure (`internal` / "handler exploded").
  - `tool_error` on permission deny (`permission_denied`).
  - `tool_error` on ask short-circuit (`permission_denied`).
  - `tool_error` on broker rejection (`permission_denied`).
  - `tool_error` suppressed on the allow happy path.
  - A four-event ordering case under a failing handler that subscribes
    to all four events and asserts the captured order is exactly
    `tool_before → tool_dispatched → tool_error → tool_after`.

  The slice-22 hook tests stay untouched and remain green — they
  subscribe only to `tool_before` + `tool_after`, so the new
  `tool_dispatched` / `tool_error` publishes that fire on the same
  calls go to zero subscribers (cheap publish-to-empty path) and the
  capture counts assert the same `2`. `test-tool` grows to **74 cases
  / 599 assertions** (+11 cases / +83 assertions). All ten test
  buckets stay green.

  `tests/hook/test_bus.cpp` needs one mechanical extension: the
  `payload_kind` visitor inside the recording sink does an
  `if constexpr` on each variant alternative and returns a stable
  string. With two new alternatives the compiler rejects the visitor
  for missing branches; two new branches (`"dispatched"` /
  `"error"`) restore exhaustiveness. The bus tests themselves do not
  need new cases — slice-22's bus tests cover the publish mechanics,
  and slice-25's dispatch tests cover the new events end-to-end.
- **Bench.** No new scenarios. The slice-22
  `dispatch_allow_no_hooks` / `with_empty_bus` / `with_two_sinks`
  trio already measures the per-publish overhead; this slice adds one
  extra publish (`tool_dispatched`) on the allow path, and the
  measured deltas stay within noise (`with_empty_bus` rose ~85 ns,
  within the bench's 5-7% err% — the publish-to-empty-subscribers
  path is ~242 ns per the existing `bench-hook/publish_no_sinks`
  baseline). No need for a new dedicated A/B; the existing trio is the
  load-bearing measurement.

### Design Intent

**Why distinct typed payloads vs. reusing `ToolBeforePayload` /
`ToolAfterPayload`.** The `Bus` discriminates by `Event`, not by
variant alternative — a sink that subscribes to `tool_dispatched` and
receives a `ToolBeforePayload` variant could still read the fields it
needs. But distinct types make the contract explicit at compile time,
let the visitor in `payload_kind` produce a unique string, and let
future fields diverge (the `verdict` field on `ToolDispatchedPayload`
has no equivalent on `ToolBeforePayload`; the `succeeded` /
`output_text` columns on `ToolAfterPayload` have no equivalent on
`ToolErrorPayload`). The variant grew from three to five alternatives
— the discrimination cost is one extra tag byte and one more
`std::visit` branch per existing visitor, both negligible.

**Why `verdict` on `ToolDispatchedPayload` carries the rule's verdict,
not the audit row's outcome.** The audit outcome already promotes
`ask` to `approved` when the broker accepts the call; that signal
lives on the audit row and is the canonical place for "was this a
broker-approved call". The event payload's `verdict` answers a
different question — "what verdict from the rule set led the
dispatcher to call the handler" — and keeping it the raw rule
verdict makes the slice-22 / slice-25 split self-consistent: every
event payload that carries a verdict-like field uses the same
spelling. The promotion-to-`approved` is visible to anyone with the
audit sink (which the same dispatcher feeds) without the hook payload
having to mirror it.

**Why `tool_dispatched` fires after audit and before the handler, and
only when the handler will actually run.** The semantic the design
doc gives this event is "the call has cleared every gate and is
about to execute" — that's exactly the post-audit, pre-handler
edge. Firing it for deny / ask-short-circuit / broker-reject would
betray the semantic and force every subscriber to filter on the
outcome (which is the `tool_after` / `tool_error` job). Firing only
on the paths where the handler runs means a future rate-limiter sink
can react to "we're about to invoke" by, e.g., decrementing a token
bucket without having to wonder whether the call was actually
permitted.

**Why `tool_error` shares the same `finished_at` as `tool_after` on
the failure path.** The two events fire one after the other in a
single dispatcher tick; the existing `tool_after` publish already
captures `core::time::now_utc()` at this point. Sampling the clock
twice would produce two distinct timestamps for the same logical
"the call ended" moment, which is misleading for any sink that wants
to correlate them by timestamp. Sharing the value is one stack
variable that survives until both publishes have finished.

**Why no new bench scenario.** The slice-22 trio already varies the
sink configuration along the dispatch path. The slice-25 change adds
one extra publish on the allow path, and the existing
`dispatch_allow_with_empty_bus` is exactly the measurement of "the
no-op cost of a publish to zero subscribers" — the new publish lands
on that path and the bench picks it up. A dedicated A/B comparing
"three publishes" vs. "two publishes" would measure the same single
publish twice; the slice-22 baseline already pins it (~242 ns per
`bench-hook/publish_no_sinks`). The optimisation order rule
([`bench-criterion-and-optimization-order`](.../memory/bench-criterion-and-optimization-order.md))
applies here — bench only to resolve uncertainty, and there is none.

### Files Modified

- `include/oran/hook/payload.hpp` — file header refreshed (drop the
  "slice 22 only publishes before/after" caveat); new
  `ToolDispatchedPayload` (5 fields, `verdict` is the rule's verdict
  wire spelling); new `ToolErrorPayload` (7 fields, failure-only);
  `Payload` variant grows from 3 → 5 alternatives.
- `src/oran-tool/registry.cpp` — two new helpers
  (`build_dispatched_payload`, `build_error_payload`); the post-audit
  `handler_about_to_run` branch publishes `tool_dispatched`; the
  pre-`tool_after` block conditionally publishes `tool_error`. Both
  publish sites are gated on `ctx.bus != nullptr` so the null-bus
  contract is preserved. Includes already pulled in
  `<oran/hook/event.hpp>` + `<oran/hook/payload.hpp>` in slice 22.
- `tests/tool/test_registry.cpp` — `CapturedEvent` grows two fields
  (`error_message`, `verdict`); the `CaptureSink` visitor extends the
  `if constexpr` ladder with the two new variant alternatives; eleven
  new `[unit][tool][hook]` cases (~250 LoC total).
- `tests/hook/test_bus.cpp` — `payload_kind` visitor extends the
  `if constexpr` ladder with `"dispatched"` and `"error"`; the
  `Capture::payload_kind` field comment refreshed accordingly.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 24 → 25.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 25, history pointer, `oran-tool` library
  surfaces row (74 cases / 599 assertions), refreshed `Next intended
  slice` to drop the now-landed item, removed the closed tech-debt
  row from the open list.
- `docs/QUALITY_SCORE.md` — Test framework row refreshed with the
  new `oran-tool` counts; Tool registry row gains the slice-25
  publish + new test paths; Hooks row gains the new typed payloads,
  the four-event dispatcher contract, the new test coverage, and
  drops the now-landed item from "Next Step".
- `docs/ARCHITECTURE.md` — slice-status preamble lists slice 25
  alongside the prior slices and the `oran-tool` inventory row
  documents the four-event tap; the planned-events list trimmed.
- `docs/exec-plans/tech-debt-tracker.md` — `tool_dispatched` /
  `tool_error` row removed (closed by this slice). The advisory-only
  blocking row stays (still has no consumer); the `file.search`
  ripgrep-class row stays (still gated on a real workload measurement).
- `docs/releases/feature-release-notes.md` — new top row
  `oran-tool-lifecycle-events`.
- `docs/histories/2026-05/20260520-1900-tool-lifecycle-events.md` —
  this file.

### Validation

- Commands run:
  - `xmake build oran-tool` — clean (~11.7 s).
  - `xmake build test-tool` — clean (~39 s).
  - `./build/linux/x86_64/release/test-tool` — 74 cases / 599
    assertions, all green.
  - `./build/linux/x86_64/release/test-tool "[hook]"` — 19 cases /
    146 assertions, all green.
  - `xmake test` — all 10 buckets green (test-async / bootstrap /
    cli / config / core / hook / io / permission / storage / tool).
  - `xmake build bench-tool && xmake run bench-tool` — clean;
    measured `dispatch_allow_no_hooks` ~2.0 µs vs.
    `dispatch_allow_with_empty_bus` ~2.5 µs vs.
    `dispatch_allow_with_two_sinks` ~2.9 µs (the slice-22 trio
    within noise post-slice-25). Other scenarios unchanged.
  - `xmake build orangutan && ./build/linux/x86_64/release/orangutan --help`
    — first line reads `orangutan v2.0.0-slice25`.
- Tests added/changed: 11 new tool-bucket cases (+83 assertions); the
  `payload_kind` visitor in `tests/hook/test_bus.cpp` extended for
  exhaustiveness.
- Bench impact: existing scenarios within noise. No new scenarios —
  see "Design Intent" above for why.
- Compile-budget delta: zero new includes on either side of the
  registry split (`<oran/hook/payload.hpp>` already in the
  include set). No PCH change.

### Follow-ups

- Issues to file: none.
- Tech-debt entries filed: none. The closed row leaves two open in
  the tracker — advisory-only hook bus (gated on first blocking
  consumer in `oran-agent`) and `file.search` ripgrep-class
  optimisations (gated on a real workload measurement).
- Linked release note: 2026-05-20 `oran-tool-lifecycle-events` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: the `tool_dispatched` payload's
  `verdict` field is the first place a hook event mirrors a state the
  audit row also captures — when the agent loop lands and starts
  consuming both surfaces, look for opportunities to collapse the
  duplication (a sink that wants `verdict` could read it from the
  audit row that the same dispatcher just wrote, but only if the
  audit sink hands back a synchronous reference — today's async
  `record` returns `Awaitable<Result<void>>` so the hook publish
  needs its own copy). The `handler_about_to_run` helper boolean is
  a one-line summary of "did the dispatch resolve to handler entry",
  which the blocking-hook slice will also want when it short-circuits
  on a `tool_before` veto — extract it to a helper at that point.

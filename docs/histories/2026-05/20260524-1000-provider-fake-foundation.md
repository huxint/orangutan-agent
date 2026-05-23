## [2026-05-24 10:00] | Task: fake-provider foundation

### Execution Context

- Agent: `Claude Code (Opus 4.7)`
- Base model: `claude-opus-4-7`
- Runtime: `local workspace / xmake`
- Linked plan: none — `docs/STATUS.md` "Next intended slice" pointed at the
  fake-provider-first loop foundation, and the slice closes the spec 0017
  System / FakeProvider / EventSink prework in a single history.

### User Query

Continue advancing the rewrite after the slice-73 provider cache-hint shipment,
keeping one coherent version per commit and syncing docs with the code.

### Changes Overview

- Areas: `oran-provider` (new `System` / `EventSink` / `FakeProvider`
  surface), provider tests, build dependency hygiene, provider/loop docs.
- Key actions: added the `provider::System` abstract base and `EventSink`
  streaming observer, added the `ProtocolKind` / `ModelTarget` / `Route`
  value shapes, added `provider::FakeProvider` with `ScriptedTurn` /
  `StreamDelta` and cancel-aware scripted latency, wired `oran-async` into
  the `oran-provider` dep set, and exercised the new surface from
  `test-provider` with eight new cases.

### Design Intent

Spec 0017 freezes the v1 sequencing as fake-provider-first. Slice 73 shipped
the provider-domain value types and the cache-hint mapper; this slice closes
the remaining provider prework — `System`, `EventSink`, `Route`, and
`FakeProvider` — so the next slice can author the `agent::Loop` against
deterministic shapes instead of a vendor adapter. The fake provider lives
inside `oran-provider` rather than a sibling library: the cost of the small
test fixture inclusion is lower than the boundary tax of a separate target,
and the public header surface stays `nlohmann`-free. The `System::send`
signature is `const`-qualified per the design-doc sketch, so future
multi-agent processes can share one provider instance across concurrent
turns; the fake's plan cursor lives behind a `mutable std::mutex` so the
contract holds without changing the public surface.

`StreamDelta` is a `std::variant` whose alternatives name exactly one
`EventSink` callback so test plans double as sink-call expectation lists.
Delta assembly opens a new `TextContent` / `ThinkingContent` block whenever
the trailing block kind differs and accumulates `ToolUseContent::input_json`
by matching `id` on the most recent open tool block, matching the streaming
shape every supported vendor adapter will need to satisfy. Scripted latency
runs through `async::sleep_for` so parent cancellation interrupts the wait
in line with rule C11.

### Files Modified

- `include/oran/provider.hpp`
- `include/oran/provider/system.hpp`
- `include/oran/provider/fake.hpp`
- `src/oran-provider/fake.cpp`
- `tests/provider/test_fake.cpp`
- `xmake/targets.lua`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice/history pointer, provider description, latest
  provider test count, and next intended slice updated.
- `docs/ARCHITECTURE.md` — `oran-provider` inventory now reflects the
  shipped `System` / `EventSink` / `Route` / `FakeProvider` surface and the
  new `oran-async` dependency.
- `docs/design-docs/api-portability.md` — domain-model and execution-layer
  status notes now name the shipped `FakeProvider` / `EventSink`.
- `docs/design-docs/agent-platform.md` — prompt assembly + loop status now
  acknowledges that the provider side has a deterministic harness.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — status
  section moves `System` / `EventSink` / `FakeProvider` from "still
  planned" to shipped, leaving `agent::Loop` and the ten scenarios next.
- `docs/QUALITY_SCORE.md` — provider row updated.
- `docs/releases/feature-release-notes.md` — release note added.

### Validation

- Commands run:
  - `xmake build oran-provider`
  - `xmake build test-provider`
  - `xmake run test-provider`
  - `xmake build`
  - `xmake test`
  - `bash scripts/check-deps.sh`
- Tests added/changed: `test-provider` now reports 11 cases / 89 assertions
  (was 3 cases / 25 assertions). The full `xmake test` run is green across
  13 test buckets.
- Bench impact: no benches added or changed in this slice; the existing
  `bench-provider` cache-mapping scenarios remain the perf reference for
  the provider library. A `bench-provider/fake_provider` scenario is
  intentionally deferred until the agent loop produces a realistic
  workload measurement.
- Compile-budget delta: no budget row changed; `oran-provider` already has
  a documented provider/prompt/agent TU category, and the new headers
  remain `nlohmann`-free at the public surface.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
  (`provider-fake-foundation`).

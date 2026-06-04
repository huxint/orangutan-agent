## [2026-06-04 21:36] | Task: slice 158 — hook payload sharing

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `OpenAI API / local xmake`
- Linked plan: none — small deep-review tracker cleanup slice.

### User Query

> Deeply understand the architecture and current progress, then start the next
> implementation slice and commit it.

### Changes Overview

- Areas: `oran-hook`, hook API consumers, hook bench/docs, deep-review tracker.
- Key actions:
  - Added `hook::PayloadPtr = std::shared_ptr<const Payload>` and changed
    `Sink::receive`, `Sink::handle_blocking`, and `InProcessSink` callbacks to
    receive the shared immutable payload handle.
  - Reworked `hook::Bus` delivery to build at most one trusted/raw payload
    snapshot and one default/redacted payload snapshot per publish, then share
    those snapshots across advisory and blocking sinks.
  - Added a regression test proving default sinks in one advisory publish share
    the same redacted snapshot while trusted-local sinks receive a distinct raw
    snapshot.
  - Added large redacted `tool_after` payload bench scenarios for 1 vs. 3
    default sinks.

### Design Intent

The 2026-05-21 deep-review row carried `shared_ptr<const Payload>` as a P3
multi-sink hook cleanup after advisory fan-out shipped. The old bus cloned the
full `Payload` once per sink before redaction. That was acceptable for metadata
payloads, but wasteful for `tool_after` payloads that may carry large
`data_json` / mutation input fields only to clear them for every default sink.

The new boundary keeps producer APIs by value (`publish_advisory(Event,
Payload)`, `publish_blocking<E>(Payload)`) so callers can hand ownership to the
bus, while sink APIs receive a shared immutable handle whose lifetime is safe
across coroutine suspension. Redaction remains bus-owned: default sinks share
one redacted snapshot and trusted-local sinks share one raw snapshot.

### Files Modified

- `include/oran/hook/payload.hpp`
- `include/oran/hook/sink.hpp`
- `include/oran/hook/in_process_sink.hpp`
- `include/oran/hook/bus.hpp`
- `src/oran-hook/bus.cpp`
- `src/oran-hook/in_process_sink.cpp`
- `src/oran-hook/sink.cpp`
- `include/oran/cli/operator_prompt_sink.hpp`
- `src/oran-cli/operator_prompt_sink.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/hook/test_bus.cpp`
- `tests/hook/test_in_process_sink.cpp`
- `tests/hook/test_publish_blocking.cpp`
- `tests/tool/test_registry.cpp`
- `tests/agent/test_loop.cpp`
- `tests/agent/test_scheduler.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `bench/hook/scenarios/bus.cpp`
- `bench/tool/scenarios/hooks.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/permissions-and-hooks.md` — sink API now uses
  `PayloadPtr`; bus delivery shares raw/default-redacted snapshots; public
  bus sketch uses the current `bind` / `publish_advisory` names.
- `docs/design-docs/cli-runtime.md` — `OperatorPromptSink` sketch now accepts
  `hook::PayloadPtr`.
- `docs/product-specs/0014-structured-tool-output.md` — hook fan-out redaction
  text now describes shared raw/default snapshots instead of per-sink copies.
- `docs/product-specs/0015-blocking-hook-decisions.md` — blocking hook spec
  status and delivery contract updated for `PayloadPtr`.
- `docs/ARCHITECTURE.md` — `oran-hook` inventory row updated for shared
  payload delivery.
- `bench/hook/README.md` — new large redacted payload bench scenarios.
- `docs/QUALITY_SCORE.md` — hook test counts and local bench figures.
- `docs/exec-plans/tech-debt-tracker.md` — closes the P3 multi-sink payload
  sharing item from the 2026-05-21 deep-review row.
- `docs/STATUS.md` — slice/version and latest-history pointer.

### Validation

- Commands run:
  - `for t in test-hook test-tool test-agent test-bootstrap test-cli bench-hook bench-tool; do xmake build "$t" || exit $?; done`
  - `for t in test-hook test-tool test-agent test-bootstrap test-cli; do xmake run "$t" || exit $?; done`
  - `for t in bench-hook bench-tool; do xmake run "$t" || exit $?; done`
  - `xmake test`
  - `make ci`
- Tests added/changed: `test-hook` adds the shared-redacted snapshot regression
  and now reports 34 cases / 243 assertions.
- Bench impact: local `bench-hook` reports advisory no/one/three sink fan-out
  at ~309 ns / ~1.57 us / ~3.77 us; blocking no/one/three/short-circuit at
  ~390 ns / ~2.80 us / ~6.91 us / ~4.80 us; large redacted payload delivery
  at ~4.05 us for one default sink and ~6.58 us for three default sinks.
- Compile-budget delta: not measured; no new dependency and public header adds
  only `<memory>`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: closed the `shared_ptr<const Payload>` P3 item in
  `review/deep-2026-05-21`; remaining P3 items are `Runtime::Impl::run()`
  clarification/refactor and the vector backend trait / `sqlite-vec` adapter.
- Linked release note: none; this is an internal API/runtime cleanup, not a
  user-visible CLI feature.

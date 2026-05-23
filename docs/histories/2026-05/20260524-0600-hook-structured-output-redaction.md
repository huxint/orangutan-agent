## [2026-05-24 06:00] | Task: Hook Structured Output Redaction

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `none — narrow spec-0014 hook-redaction slice`

### User Query

Continue the long-running Orangutan v2 implementation by reading the project
docs first, moving one coherent version forward, keeping docs in sync, validating
the result, and committing it as its own version. This docs-sync pass was asked
to own only docs/history files after the parent landed the hook/tool behavior.

### Changes Overview

- Areas: `oran-hook` sink capability surface, `oran-tool` structured-output
  hook fan-out, structured tool output docs, release/status docs.
- Key actions: `hook::Sink` now exposes `kind()` with `SinkKind::default_`
  as the default and `SinkKind::trusted_local` as the explicit trusted-local
  classification. `hook::InProcessSink` can be constructed with either kind.
- Key actions: `ToolAfterPayload` now has optional `data_json`, and
  `Registry::dispatch` copies successful `tool::Output::data_json` into that
  hook payload. `hook::Bus::publish_advisory` then builds per-sink payload
  copies and clears `ToolAfterPayload::data_json` for every non-trusted-local
  sink, so default sinks keep the existing text + usage view while trusted-local
  in-process observers can receive the raw structured bytes.
- Key actions: `src/oran-bootstrap/bootstrap.cpp` bumps `kVersion` from
  `2.0.0-slice64` to `2.0.0-slice65`, so `xmake run orangutan` reports the
  new slice identity once the parent code is built.

### Design Intent

Spec 0014 intentionally separates model-facing text from structured JSON bytes.
The hook bus is an observability surface, but not every hook sink is equally
trusted: shell/webhook-style sinks can cross process or network boundaries, while
trusted in-process sinks are local runtime extensions. The slice keeps the public
payload useful for everyone by always delivering `output_text` and `usage`, but
requires an explicit `SinkKind::trusted_local` opt-in before raw `data_json`
leaves the bus. The redaction lives in `oran-hook` rather than in each producer
so future `tool_after` publishers get the same policy without duplicating it.

### Files Modified

- `include/oran/hook/sink.hpp`
- `include/oran/hook/payload.hpp`
- `include/oran/hook/in_process_sink.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-hook/bus.cpp`
- `src/oran-tool/registry.cpp`
- `tests/hook/test_bus.cpp`
- `tests/hook/test_in_process_sink.cpp`
- `tests/tool/test_registry.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice/version pointer, current spec-0014 summary,
  remaining-work list, and focused `oran-hook` / `oran-tool` test counts.
- `docs/ARCHITECTURE.md` — current `oran-hook` / `oran-tool` inventory now
  records `SinkKind`, trusted-local delivery, and default redaction.
- `docs/design-docs/permissions-and-hooks.md` — hook bus and sink-kind policy
  now document `SinkKind::trusted_local` and the `ToolAfterPayload::data_json`
  redaction boundary.
- `docs/design-docs/tool-runtime.md` — Output Shape v2 now records that
  raw `data_json` hook redaction shipped in slice 65.
- `docs/product-specs/0014-structured-tool-output.md` — status, hook fan-out,
  and acceptance criterion 6 now mark hook redaction shipped while keeping
  provider mapping, scheduler byte caps, and audit usage fan-out open.
- `docs/QUALITY_SCORE.md` — hook/tool summaries and focused test counts now
  reflect the new public surface and regressions.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build test-hook`
  - `xmake build test-tool`
  - `xmake run test-hook`
  - `xmake run test-tool "[unit][tool][hook][output]"`
  - `xmake build orangutan`
  - `xmake run orangutan` (reports `orangutan v2.0.0-slice65`)
  - `make check-docs`
  - `scripts/check-status-fresh.sh`
  - `git diff --check`
  - `xmake test`
  - `make ci`
- Tests added/changed: parent added hook and tool regressions:
  `tests/hook/test_bus.cpp` covers default-sink redaction vs.
  trusted-local delivery, `tests/hook/test_in_process_sink.cpp` covers default
  and trusted kinds, and `tests/tool/test_registry.cpp` covers structured-output
  copy plus default/trusted hook fan-out. Focused outputs reported
  `oran-hook` 17 cases / 109 assertions and `oran-tool` 157 cases /
  1485 assertions.
- Bench impact: no new bench scenario; the hot-path cost is one optional
  string copy for successful structured outputs and per-sink payload copies
  when `tool_after` carries raw data.
- Compile-budget delta: public hook headers add one small enum, one virtual
  getter, and one optional string field; no heavy JSON include is introduced.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`

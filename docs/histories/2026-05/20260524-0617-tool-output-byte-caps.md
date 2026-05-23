## [2026-05-24 06:17] | Task: Tool Output Byte Caps

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `none — narrow spec-0014 byte-cap slice`

### User Query

Continue the long-running Orangutan v2 implementation by reading the project
docs first, moving one coherent version forward, keeping docs in sync,
validating the result, and committing it as its own version.

### Changes Overview

- Areas: `oran-tool` structured-output boundary, `oran-config` runtime
  config, `bench-tool`, status/release docs.
- Key actions: `<oran/tool/output.hpp>` now exposes `OutputCapOptions`,
  `OutputCapReport`, and `apply_output_caps`. The helper truncates
  over-budget `Output::text` at a UTF-8 code-point boundary and sets
  `usage.truncated`; it drops over-budget `Output::data_json`, keeps the
  text fallback, and sets `usage.data_dropped`.
- Key actions: `DispatchContext` now carries `output_caps`, and
  `Registry::dispatch` applies them after a successful handler return and
  before it returns the output or publishes `tool_after`. Direct registry
  callers therefore get spec-0014 defaults even before the scheduler exists.
- Key actions: `oran-config` parses `runtime.tool_output.max_text_bytes` and
  `runtime.tool_output.max_data_bytes`, with `config.example.json`
  documenting the 256 KiB / 1 MiB defaults. The future scheduler/agent owner
  can thread those parsed values into `DispatchContext::output_caps`.
- Key actions: `src/oran-bootstrap/bootstrap.cpp` bumps `kVersion` from
  `2.0.0-slice65` to `2.0.0-slice66`.

### Design Intent

Spec 0014 says output caps belong at the scheduler boundary, not in each
handler. The scheduler does not exist yet, but direct `Registry::dispatch`
already returns provider/hook-facing output, so this slice adds one shared
cap helper and applies it at that boundary. That avoids duplicating cap logic
inside built-ins, preserves the current pre-effect audit row ordering, and
leaves a clean migration path: when `oran-agent` lands, the scheduler owns the
same options and calls the same helper for batched tool results.

The config fields land now because they are the documented operator surface
for the defaults. They are parsed and tested, but not yet wired into a
long-lived runtime owner because that owner is still future `oran-agent` /
scheduler work.

### Files Modified

- `include/oran/tool/output.hpp`
- `src/oran-tool/output.cpp`
- `include/oran/tool/registry.hpp`
- `src/oran-tool/registry.cpp`
- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `config.example.json`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/tool/test_output.cpp`
- `tests/tool/test_registry.cpp`
- `tests/config/test_config.cpp`
- `bench/tool/scenarios/output.cpp`
- `bench/tool/README.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice/version pointer, last-history pointer, next-slice
  summary, and config/tool test counts.
- `docs/ARCHITECTURE.md` — current `oran-config` / `oran-tool` inventory now
  records runtime tool-output caps, cap helper types, dispatch-boundary
  application, and the remaining provider/audit follow-ups.
- `docs/design-docs/tool-runtime.md` — Output Shape v2 now documents
  `OutputCapOptions`, `apply_output_caps`, dispatch-boundary application, and
  the new bench scenario.
- `docs/product-specs/0012-tool-scheduler-and-state.md` — scheduler
  cross-reference now points at the shipped helper and clarifies that future
  batched scheduling owns the options.
- `docs/product-specs/0014-structured-tool-output.md` — status, byte-cap
  policy, acceptance criteria, and bench criteria now mark byte caps shipped.
- `docs/design-docs/secrets-and-state.md` — config typed-field status now
  includes `runtime.tool_output`.
- `docs/QUALITY_SCORE.md` — config/tool test counts and bench note updated.
- `bench/tool/README.md` — output bench inventory now lists
  `output.apply_caps`.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake build test-config`
  - `xmake run test-tool "[unit][tool][output]"`
  - `xmake run test-tool "[unit][tool][hook][output]"`
  - `xmake run test-config "[unit][config][runtime]"`
  - `xmake run test-config`
  - `xmake build orangutan`
  - `xmake build bench-tool`
  - `xmake run orangutan` (reports `orangutan v2.0.0-slice66`)
  - `xmake run test-tool`
  - `make check-docs`
  - `scripts/check-status-fresh.sh`
  - `xmake test`
  - `make ci`
  - `git diff --check`
- Tests added/changed: `tests/tool/test_output.cpp` covers UTF-8-safe text
  caps, data dropping, and disabled zero caps; `tests/tool/test_registry.cpp`
  pins dispatch-time caps before `tool_after`; `tests/config/test_config.cpp`
  covers parsed `runtime.tool_output` fields and malformed values. Focused
  outputs reported `oran-config` 26 cases / 184 assertions and `oran-tool`
  161 cases / 1515 assertions.
- Bench impact: `bench-tool` adds `output.apply_caps` for the shared cap
  helper.
- Compile-budget delta: one small `src/oran-tool/output.cpp` TU and public
  stdlib-only types in `<oran/tool/output.hpp>`; no heavy JSON include is
  introduced.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`

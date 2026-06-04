## [2026-06-04 11:23] | Task: trace retention enforcement

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local shell in repository checkout`
- Linked plan: none; this is a small storage/bootstrap follow-up named by
  `QUALITY_SCORE.md` after the memory/skills arc.

### User Query

> Deeply understand the project architecture and current progress before
> starting the next slice.

### Changes Overview

- Areas: `oran-storage`, `oran-bootstrap`, trace/runtime docs.
- Key actions: added `TraceRepository::purge_turns_started_before(...)`,
  added `RuntimeAssemblyOptions::trace_retention_started_before_ns`, and had
  `bootstrap::run` derive that explicit cutoff from `config.trace().retention_days`
  before assembly build. The binary banner moved to slice 150.

### Design Intent

`trace.retention_days` was already parsed but not consumed. This slice keeps
storage deterministic by accepting an explicit Unix-nanosecond cutoff rather
than reading a clock in `oran-storage`, while bootstrap owns the config-to-clock
translation at startup. The purge deletes only `trace_turns`; audit rows remain
durable because audit retention is a separate policy.

### Files Modified

- `include/oran/storage/trace_repository.hpp`
- `src/oran-storage/trace_repository.cpp`
- `include/oran/bootstrap/runtime_assembly.hpp`
- `src/oran-bootstrap/runtime_assembly.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/storage/test_trace_repository.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — storage/config/bootstrap inventory rows mention trace retention.
- `docs/design-docs/storage-runtime.md` — documented the purge API and audit-row separation.
- `docs/design-docs/bootstrap-runtime.md` — documented the runtime assembly cutoff option.
- `docs/design-docs/secrets-and-state.md` — recorded runtime consumption of `trace.retention_days`.
- `docs/product-specs/0018-first-loop-observability.md` — updated operator-config status.
- `docs/QUALITY_SCORE.md` — refreshed counts and storage/config/bootstrap next steps.
- `docs/STATUS.md` — moved the snapshot to slice 150 and recorded validation.
- `docs/releases/feature-release-notes.md` — added the operator-facing trace-retention note.

### Validation

- Commands run:
  - `xmake build test-storage`
  - `xmake build test-bootstrap`
  - `xmake run test-storage`
  - `xmake run test-bootstrap`
- Tests added/changed: storage coverage proves strict-cutoff purge semantics and
  invalid cutoff validation; bootstrap coverage proves assembly startup applies
  retention before exposing the trace repository. One existing prompt-runner
  fixture now uses a 3 s async timeout because the canonical console reporter
  path intermittently exceeded the default 1 s timeout even though the case
  passed repeatedly with a compact reporter.
- Bench impact: none; retention is startup maintenance work.
- Compile-budget delta: not measured; touched TUs remain within the focused
  build/test path.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`.

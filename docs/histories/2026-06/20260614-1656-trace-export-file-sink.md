## [2026-06-14 16:56] | Task: trace export file sink

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none; this stayed inside the small Observability v1.1 trace-export slice.

### User Query

> Continue iterating the project slice by slice after reading the development docs,
> choose the most valuable current slice instead of writing arbitrary future status,
> and commit with a detailed conventional message.

### Changes Overview

- Areas: `oran-bootstrap`, trace export, operator docs.
- Key actions: added `--trace-export-file <path>` to the existing
  `--trace-export` operator command; the file sink works for both a known
  `<turn-id>` and bounded list mode, writes the same redacted JSON Lines
  sequence stdout mode would emit, creates parent directories, truncates the
  target file, and suppresses stdout.
- Validation now rejects duplicate file-sink flags, missing values, empty
  paths, and `--trace-export-file` without `--trace-export`.
- Tests cover single-turn file output, bounded list file output, stdout
  suppression, and the new argument failures. The stdout capture helper now
  flushes before redirecting so earlier human-readable `--trace` output cannot
  leak into later JSONL assertions.

### Design Intent

Spec 0018 v1.1 already named stdout, file, and HTTP export sinks. Slice 240 made
the query itself useful by adding bounded list mode, so the next highest-value
unblocked step was a file sink over the same query rather than a broader query
language. Keeping the sink inside bootstrap preserves the current operator-owned
one-shot shape: it is read-only against `audit.db`, uses the same migration and
trace/audit redaction contract as stdout export, and treats the file path as an
explicit operator sink path rather than a runtime tool action. HTTP POST remains
the next concrete export sink because it can reuse the same JSONL rendering path.

### Files Modified

- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_bootstrap.cpp`
- `README.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/ROADMAP.md`
- `docs/STATUS.md`
- `docs/RELIABILITY.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/product-specs/0018-first-loop-observability.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `README.md` — adds trace-export file examples to Quick Start.
- `docs/product-specs/0018-first-loop-observability.md` — marks file output
  shipped for AC13 and keeps HTTP POST downstream.
- `docs/RELIABILITY.md` — documents the file sink's operational behavior.
- `docs/design-docs/bootstrap-runtime.md` — documents the bootstrap-owned
  trace export forms and file sink.
- `docs/ARCHITECTURE.md` — updates the `oran-bootstrap` binary inventory.
- `docs/ROADMAP.md` — advances the Observability frontier and next step.
- `docs/STATUS.md` — bumps the slice, history pointer, summary, next intended
  slice, and bootstrap test counts.
- `docs/QUALITY_SCORE.md` — refreshes bootstrap counts and Observability state.
- `docs/releases/feature-release-notes.md` — adds the user-visible release row.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap "[trace]" --reporter=console --verbosity=normal`
  - `build/linux/x86_64/release/test-bootstrap --reporter=console --verbosity=normal`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed: `test-bootstrap` now covers single-turn and bounded-list
  `--trace-export-file` output plus file-sink argument validation; `[trace]`
  reports 16 cases / 230 assertions and default `test-bootstrap` reports
  153 cases / 1484 assertions.
- Bench impact: none; this is a one-shot operator CLI path, not a hot path.
- Compile-budget delta: one bootstrap TU include (`<fstream>`) and small helper
  logic; no public headers or templates changed.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`

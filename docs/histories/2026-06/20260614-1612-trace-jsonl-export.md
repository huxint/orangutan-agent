## [2026-06-14 16:17] | Task: trace JSONL export

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI/API, xmake release build
- Linked plan: none; this is a small Observability v1.1 slice below the
  execution-plan threshold. The active QQ port plan remains open but its next
  gate requires real QQ credentials plus a sendable operator conversation.

### User Query

Continue the long-running slice workflow with an effectively unbounded budget:
read the development docs first, understand real project progress, choose the
highest-value current slice instead of inventing a status-only next slice, then
complete implementation, validation, docs/history sync, and a detailed
Conventional Commit.

### Changes Overview

- Areas: bootstrap CLI, trace observability, bootstrap tests, status/docs.
- Key actions:
  - Added `orangutan --trace-export <turn-id>` and
    `--trace-export=<turn-id>`.
  - Kept `--trace` and `--trace-export` mutually exclusive and reused the same
    32-character lowercase hex turn-id validation.
  - Refactored the trace inspector read path into a shared read-only helper
    that opens `<workspace>/.orangutan/audit.db`, runs the idempotent audit
    migration, reads one `trace_turns` row, and joins audit rows by
    `parent_turn_id`.
  - Emitted one JSON Lines object with `kind=trace_turn`, trace fields,
    parsed `context_json`, and ordered joined audit rows with parsed
    `metadata_json`.
  - Bumped the binary slice tag to `2.0.0-slice239`.

### Design Intent

QQ-port milestone 4b-ii is the next Channels gate, but the local environment
still lacks the real QQ credentials and operator conversation needed to run it.
The highest-value unblocked slice was therefore Observability: it is the lowest
quality-score area and spec 0018 already names JSON Lines trace export as the
first v1.1 direction.

This slice deliberately starts with stdout and a single known turn id. That
keeps the command a read-only sibling of `--trace`, avoids adding a daemon,
file sink, HTTP sink, or query language prematurely, and still gives operators
a machine-ingestible shape without scraping the human table output. The export
uses structured JSON construction rather than string concatenation; if older
rows contain non-JSON `context_json` / `metadata_json`, the exporter preserves
the raw string instead of failing the whole command.

### Files Modified

- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_bootstrap.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped the slice to 239, recorded the trace JSONL export,
  refreshed bootstrap counts, and kept QQ 4b-ii blocked on real credentials.
- `docs/ROADMAP.md` - moved the Observability frontier to slice 239 and named
  bounded multi-turn export as the next unblocked step.
- `docs/QUALITY_SCORE.md` - moved Observability from D to C with the current
  trace/export reality and refreshed bootstrap counts.
- `docs/ARCHITECTURE.md` - refreshed the `oran-bootstrap` inventory with the
  slice-239 exporter and shared signal-aware one-shot trace drain.
- `docs/design-docs/bootstrap-runtime.md` - documented the `--trace-export`
  sibling command next to the existing `--trace` inspector.
- `docs/product-specs/0018-first-loop-observability.md` - documented the
  slice-239 stdout/single-turn trace export status and acceptance coverage.
- `docs/RELIABILITY.md` - added operational notes for `--trace-export` and its
  redaction boundary.
- `docs/releases/feature-release-notes.md` - added the slice 239 release note.

### Validation

- Commands run:
  - `git diff --check` - passed.
  - `xmake build test-bootstrap` - passed.
  - `build/linux/x86_64/release/test-bootstrap "run --trace-export prints one JSON Lines trace object" --reporter=console --verbosity=normal`
    - passed, 1 case / 38 assertions.
  - `build/linux/x86_64/release/test-bootstrap "[trace]" --reporter=console --verbosity=normal`
    - passed, 12 cases / 107 assertions.
  - `build/linux/x86_64/release/test-bootstrap --reporter=console --verbosity=normal`
    - passed, 149 cases / 1361 assertions; rebuilt output reports
      `orangutan v2.0.0-slice239`.
  - `xmake build orangutan` - passed.
  - `xmake run orangutan -- --help` - passed and reported
    `orangutan v2.0.0-slice239` plus the new `--trace-export <turn-id>`
    usage line.
  - `make ci` - passed.
- Tests added/changed: duplicate/conflicting trace flag coverage, missing and
  unknown `--trace-export` cases, plus JSONL output parsing for trace fields,
  parsed trace context, joined audit rows, hook-publish metadata, and NULL
  input hashes.
- Bench impact: none; the export is an operator command, not a hot turn path.
- Compile-budget delta: `nlohmann::json` is confined to the bootstrap `.cpp`
  implementation. No public header includes or library boundaries changed.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
- Next plan step: extend trace export to a bounded multi-turn operator surface
  before adding file or HTTP sinks. QQ 4b-ii remains waiting on real platform
  credentials and an operator conversation.

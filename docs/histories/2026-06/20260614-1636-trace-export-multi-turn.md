## [2026-06-14 16:36] | Task: trace export multi-turn

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
  - Kept the existing `orangutan --trace-export <turn-id>` and
    `--trace-export=<turn-id>` single-turn JSON Lines behavior.
  - Added bounded list mode:
    `orangutan --trace-export [--agent <name>] [--limit <n>]`.
  - Reused `TraceRepository::list_turns` for newest-first trace rows and
    `AuditRepository::list_events_for_turn` for per-turn cause-chain joins.
  - Emitted one redacted `kind=trace_turn` JSON Lines object per listed turn.
  - Rejected invalid limits, `--mode` on trace export, and `--agent` /
    `--limit` filters on single-turn export instead of silently ignoring them.
  - Bumped the binary slice tag to `2.0.0-slice240`.

### Design Intent

QQ-port milestone 4b-ii remains externally blocked on real QQ credentials and
an operator conversation, so the highest-value unblocked slice stayed on the
Observability track. Slice 239 made a known turn exportable; this slice makes
the same stdout JSONL shape useful for recent activity pulls without adding a
file sink, HTTP sink, query language, daemon, or new storage schema.

The list mode deliberately returns success with no output for an empty bounded
query. That matches JSON Lines ingestion expectations: each output line is a
turn object, and no matching turns means no lines. Single-turn lookup keeps the
stricter `Error::not_found` behavior because a specific requested id being
absent is operationally different from an empty list window.

### Files Modified

- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_bootstrap.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped the slice to 240, recorded bounded trace export,
  refreshed bootstrap counts, and kept QQ 4b-ii blocked on real credentials.
- `docs/ROADMAP.md` - moved the Observability frontier to slice 240 and named
  the same bounded export query's file sink as the next unblocked step.
- `docs/QUALITY_SCORE.md` - refreshed bootstrap counts and the Bootstrap /
  Observability rows with the multi-turn exporter.
- `docs/ARCHITECTURE.md` - refreshed the `oran-bootstrap` inventory with the
  slice-240 export command shape.
- `docs/design-docs/bootstrap-runtime.md` - documented the single-turn and
  bounded-list `--trace-export` forms.
- `docs/product-specs/0018-first-loop-observability.md` - updated v1.1 trace
  export status and AC13 coverage.
- `docs/RELIABILITY.md` - documented bounded trace pulls and empty-list output.
- `docs/releases/feature-release-notes.md` - added the slice 240 release note.

### Validation

- Commands run:
  - `git diff --check` - passed.
  - `xmake build test-bootstrap` - passed.
  - `build/linux/x86_64/release/test-bootstrap "run --trace-export lists bounded JSON Lines turns by agent" --reporter=console --verbosity=normal`
    - passed, 1 case / 32 assertions.
  - `build/linux/x86_64/release/test-bootstrap "run --trace-export rejects empty turn id and invalid list filters" --reporter=console --verbosity=normal`
    - passed, 1 case / 10 assertions.
  - `build/linux/x86_64/release/test-bootstrap "[trace]" --reporter=console --verbosity=normal`
    - passed, 14 cases / 164 assertions.
  - `build/linux/x86_64/release/test-bootstrap --reporter=console --verbosity=normal`
    - passed, 151 cases / 1418 assertions.
  - `xmake build orangutan` - passed.
  - `xmake run orangutan -- --help` - passed and reported
    `orangutan v2.0.0-slice240` plus
    `--trace-export [<turn-id>] [--agent <name>] [--limit <n>]`.
  - `make ci` - passed.
- Tests added/changed: bounded trace-export fixture with mixed agents,
  newest-first agent filtering, invalid limit handling, single-turn filter
  rejection, and empty-list success.
- Bench impact: none; the export is an operator command, not a hot turn path.
- Compile-budget delta: one `<charconv>` include in the bootstrap `.cpp` only;
  no public header, schema, or library dependency changes.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
- Next plan step: add a file sink for the same bounded JSONL trace-export
  query before considering an HTTP sink or broader query language. QQ 4b-ii
  remains waiting on real platform credentials and an operator conversation.

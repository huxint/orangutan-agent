## [2026-06-05 12:33] | Task: Long-term recall kind filters

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none; small follow-up from the deep-review memory tracker.

### User Query

> Start the next implementation slice, keep docs grounded in current
> architecture/progress, iterate the plan, and commit when finished.

### Changes Overview

- Areas: `oran-config`, `oran-bootstrap`, long-term memory recall policy.
- Key actions: added optional `memory.longterm.recall.kinds` parsing, validated
  configured kind names against `memory::longterm::RecordKind` before runner
  startup, passed the parsed filter into `memory::longterm::Query::kinds`, and
  bumped the binary slice tag to `2.0.0-slice166`.

### Design Intent

Slice 165 intentionally shipped only enablement plus a result limit. This slice
keeps the once-per-prompt recall boundary unchanged and uses the existing
`Query::kinds` contract instead of adding a new search path. `oran-config` stays
below `oran-memory` by storing validated non-empty string names; bootstrap owns
the dependency-aware enum validation and query mapping. Omitting `kinds`
preserves the previous all-kind non-shadow recall behavior.

### Files Modified

- `include/oran/config/config.hpp`
- `include/oran/bootstrap/prompt_runner.hpp`
- `src/oran-config/config.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `tests/config/test_config.cpp`
- `tests/bootstrap/test_bootstrap.cpp`
- `config.example.json`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/memory-system.md` — documents the shipped kind-filter policy.
- `docs/design-docs/bootstrap-runtime.md` — documents bootstrap enum validation
  and query mapping.
- `docs/design-docs/secrets-and-state.md` — updates the typed config-field list.
- `docs/product-specs/0005-memory-system.md` — updates recall scope and focused
  validation counts.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — records that kind
  filters keep recall at the prompt boundary.
- `docs/ARCHITECTURE.md` — refreshes config/memory/bootstrap inventory rows.
- `docs/QUALITY_SCORE.md` — refreshes config/bootstrap counts and remaining work.
- `docs/exec-plans/tech-debt-tracker.md` — narrows the remaining recall-policy
  tracker item now that kind filters are shipped.
- `docs/releases/feature-release-notes.md` — adds the operator-visible config
  field release note.
- `docs/STATUS.md` — bumps the current slice/history pointer and focused counts.

### Validation

- Commands run:
  - `xmake build test-config`
  - `xmake run test-config` — 43 cases / 358 assertions
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap` — 110 cases / 811 assertions
  - `make ci`
  - `git diff --check`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help` — reports `orangutan v2.0.0-slice166`
  - `xmake test` — 16/16 test buckets passed
- Tests added/changed: config parser coverage for valid/default/malformed
  `memory.longterm.recall.kinds`; bootstrap coverage for project-only recall and
  unknown kind rejection before any provider request is served.
- Bench impact: none; this is config/query plumbing, not a search implementation
  change.
- Compile-budget delta: not measured; focused rebuilds stayed within normal local
  iteration bounds.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: existing memory P3 tracker row remains for sqlite-vec,
  richer query derivation/ranking beyond kind filters, hybrid/vector search
  composition, and memory tools.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`.

## [2026-06-06 22:18] | Task: Long-term memory decay shadow boundary

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI, this checkout
- Linked plan: none

### User Query

Continue implementing the most valuable small slice in the memory runtime workstream,
following the repository's docs-first flow, avoiding benchmark-only churn, and
finishing with a conventional commit.

### Changes Overview

- Areas: `oran-memory`, bootstrap versioning, memory docs/status/release notes.
- Key actions:
  - Added `memory::longterm::DecayRequest`, `DecayResult`, request validation,
    and the `Backend::decay(...)` operation.
  - Implemented `Fts5Backend::decay(...)` as a bounded scoped shadow transition:
    visible records with `last_read_at < unused_before` and
    `importance <= importance_floor` are marked `shadow=true`, `updated_at`
    advances monotonically to `decay_at`, and FTS shadow metadata is kept in sync.
  - Added focused long-term tests for request validation, fake backend interface
    composition, default-search hiding, include-shadow inspection, already-shadow
    exclusion, scope/importance filtering, and batch limits.

### Design Intent

Slice 181 gave recall a reliable read-touch signal. This slice uses that signal
without jumping straight to automation/config ownership: it lands the smallest
library-level execution boundary a future periodic job can call. Returning the
changed records gives future `memory.decay(scope,count)` hook publishing enough
metadata without a second read, while keeping the slice independent of
`oran-automation` and bootstrap.

### Files Modified

- `include/oran/memory/longterm.hpp`
- `src/oran-memory/longterm.cpp`
- `src/oran-memory/longterm_fts5.cpp`
- `tests/memory/test_longterm.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 182 snapshot, focused validation, and next-slice candidates.
- `docs/design-docs/memory-system.md` — long-term decay API and retention-policy execution boundary.
- `docs/product-specs/0005-memory-system.md` — v1 scope and acceptance status for decay shadowing.
- `docs/ARCHITECTURE.md` — `oran-memory` public contract and remaining decay ownership gaps.
- `docs/QUALITY_SCORE.md` — `test-memory` counts and memory-tier coverage status.
- `docs/releases/feature-release-notes.md` — user-visible release note for decay shadowing.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-memory`
  - `xmake run test-memory`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
  - path-leak check over the changed files
- Tests added/changed:
  - `longterm::Fts5Backend decays stale low-importance records to shadow`
  - `longterm::Fts5Backend decay respects batch limits`
  - long-term validation and fake backend interface coverage for `DecayRequest`.
- Bench impact: none; this is correctness/policy execution plumbing, not a performance slice.
- Compile-budget delta: no new dependencies or heavy public includes.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`

## [2026-06-06 21:51] | Task: Long-term memory recall touch metadata

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
  - Added `memory::longterm::TouchRequest` plus `Backend::touch(...)` so long-term
    backends can update read metadata without conflating it with writes.
  - Implemented `Fts5Backend::touch(...)` as a scoped monotonic
    `last_read_at` update that returns the updated record, leaves `updated_at`
    unchanged, and does not rebuild indexed FTS text.
  - Wired `Runtime::recall` and `HybridRuntime::recall` to touch returned hits
    before rendering framing/data; plain `search(...)` remains read-only.

### Design Intent

Decay policy needs a reliable "last used" signal before it can distinguish stale
records from useful memories. This slice adds that metadata seam at the lexical
backend and recall-runtime boundary instead of jumping directly to policy execution.
The touch operation is intentionally separate from `upsert(...)`: reads advance
`last_read_at`, while writes still own `updated_at` and indexed text updates.

### Files Modified

- `include/oran/memory/longterm.hpp`
- `src/oran-memory/longterm.cpp`
- `src/oran-memory/longterm_fts5.cpp`
- `src/oran-memory/longterm_runtime.cpp`
- `tests/memory/test_longterm.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 181 snapshot, focused validation, and next-slice candidates.
- `docs/design-docs/memory-system.md` — long-term touch API and recall-side metadata behavior.
- `docs/product-specs/0005-memory-system.md` — v1 scope and acceptance status for recall touch metadata.
- `docs/ARCHITECTURE.md` — `oran-memory` public contract and remaining gaps.
- `docs/QUALITY_SCORE.md` — `test-memory` counts and memory-tier coverage status.
- `docs/releases/feature-release-notes.md` — user-visible release note for recall touch metadata.

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
- Tests added/changed:
  - `longterm::Runtime touches recalled hits before returning`
  - `longterm::HybridRuntime recalls with merged hybrid hits`
  - `longterm::Fts5Backend touches last_read_at without rebuilding indexed text`
- Bench impact: none; this is correctness/metadata plumbing, not a performance slice.
- Compile-budget delta: no new dependencies or heavy public includes.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`

## [2026-06-06 23:47] | Task: memory-startup-decay-diagnostics

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: none

### User Query

Continue the most valuable implementation slice using the docs-first workflow,
avoid bench-only churn, keep docs/status/history in sync, run focused
validation, and finish with a Conventional Commit.

### Changes Overview

- Areas: bootstrap runtime assembly, configured-route startup diagnostics, long-term
  memory retention.
- Key actions:
  - Added `RuntimeAssembly::longterm_memory_startup_decay_shadowed_count()`.
  - Stored the optional startup `Fts5Backend::decay(...)` shadow count on the
    assembly instead of discarding it.
  - Extended the startup banner with `startup-decay=<disabled|N>`.
  - Tightened bootstrap assembly coverage for both configured startup decay and
    disabled/no-pass cases.

### Design Intent

Slice 184 made startup retention effective but not directly observable: callers
could only infer the pass result by querying the memory DB. This slice keeps the
same execution boundary and exposes the already-computed count as a small,
stable diagnostic. `std::nullopt` means the startup pass did not run; `0` means
it ran and found no candidates. Periodic scheduling and decay hook publishing
remain separate owners.

### Files Modified

- `include/oran/bootstrap/runtime_assembly.hpp`
- `src/oran-bootstrap/runtime_assembly.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 185 snapshot, focused validation, and history pointer.
- `docs/design-docs/bootstrap-runtime.md` — public accessor and startup banner
  diagnostic.
- `docs/design-docs/memory-system.md` — startup retention diagnostics status.
- `docs/design-docs/secrets-and-state.md` — config consumption now reports a
  startup shadow count.
- `docs/product-specs/0005-memory-system.md` — memory-system scope and
  acceptance/validation notes for startup diagnostics.
- `docs/ARCHITECTURE.md` — bootstrap/memory inventory entries for the diagnostic.
- `docs/QUALITY_SCORE.md` — bootstrap assertion count and memory/bootstrap
  coverage notes.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap` — 119 cases / 1006 assertions
- Tests added/changed:
  - Tightened `RuntimeAssembly::build applies long-term startup decay before exposing memory`.
  - Tightened no-pass and disabled-memory assembly assertions for the diagnostic.
- Bench impact: no benchmark change; this is startup observability over an
  already-executed one-shot path, not a performance trade-off.
- Compile-budget delta: one public bootstrap accessor and one stored
  `std::optional<std::size_t>`; no new dependency or heavy include.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)

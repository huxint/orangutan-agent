## [2026-06-06 21:05] | Task: memory lifecycle hooks

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI, `/home/huxint/projects/orangutan-refactor`
- Linked plan: none

### User Query

> Continue the most valuable implementation slices, follow the docs-first
> repository workflow, avoid defaulting every next slice to benchmark work, and
> keep commit messages conventional.

### Changes Overview

- Areas: `oran-hook`, `oran-bootstrap`, memory-tool hook integration, docs.
- Key actions: added typed memory lifecycle hook payloads; redacted long-term
  memory records for default sinks; published blocking `memory_write_before`
  for `memory.remember`; published advisory `memory_write_after` and
  `memory_forget` after successful memory-tool mutations; pinned the proceed,
  veto, and redaction paths with focused tests.

### Design Intent

This closes the practical `memory.write.before` veto path from
`docs/product-specs/0005-memory-system.md` without making `oran-tool` depend on
`oran-memory`. The memory tools still parse, permission, audit, and dispatch
through the existing tool runtime; bootstrap remains the callback boundary that
can see the assembly-owned long-term backend, vector backend, hook bus, and
runner identity. Rewrite and require-approval decisions are deliberately
rejected as unsupported for this memory-write consumer so the first shipped
memory hook has a small, testable policy surface.

### Files Modified

- `include/oran/hook/payload.hpp`
- `src/oran-hook/bus.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `tests/hook/test_bus.cpp`
- `tests/hook/test_publish_blocking.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 179 snapshot, focused validation, and next-slice candidates.
- `docs/design-docs/permissions-and-hooks.md` — memory lifecycle payloads and redaction.
- `docs/design-docs/memory-system.md` — shipped write/forget hook boundary.
- `docs/design-docs/bootstrap-runtime.md` — runner memory-tool hook publishing.
- `docs/product-specs/0005-memory-system.md` — AC5 status and validation counts.
- `docs/product-specs/0015-blocking-hook-decisions.md` — first memory-write consumer status.
- `docs/ARCHITECTURE.md` — library inventory for hook/bootstrap/memory boundaries.
- `docs/QUALITY_SCORE.md` — hook/bootstrap/memory test counts and remaining gaps.
- `docs/releases/feature-release-notes.md` — user-visible lifecycle hook note.

### Validation

- Commands run:
  - `xmake build test-hook`
  - `xmake run test-hook`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed:
  - `tests/hook/test_publish_blocking.cpp` covers default-vs-trusted memory
    write redaction.
  - `tests/bootstrap/test_prompt_runner.cpp` covers memory write before/after
    publishes, veto skip/no-persist behavior, and memory forget advisory publish.
- Bench impact: none; this is policy/runtime lifecycle behavior, not a hot-path
  performance comparison.
- Compile-budget delta: no new dependency or target-level compile-budget change.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md#2026-06`

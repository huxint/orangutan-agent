## [2026-06-06 21:37] | Task: memory read hooks

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI, this checkout
- Linked plan: none

### User Query

> Continue the most valuable implementation slices, follow the docs-first
> repository workflow, avoid defaulting every next slice to benchmark work, and
> keep commit messages conventional.

### Changes Overview

- Areas: `oran-hook`, `oran-bootstrap`, long-term memory read observability, docs.
- Key actions: added typed memory read hook payloads; redacted recall queries and
  recalled record content for default sinks; published advisory
  `memory_read_after` after successful prompt-boundary long-term recall and
  `memory.recall` tool reads; covered the default-sink redaction path and the
  trusted-local raw-query/raw-record path.

### Design Intent

This slice closes the low-risk read observability half of the memory lifecycle
surface without expanding the blocking-hook whitelist. Successful long-term
reads now have a typed hook payload at the same bootstrap boundary that already
owns recall execution and runner identity. Default sinks receive enough metadata
to audit source, scope, kind filters, limits, match counts, scores, timing, and
record sizes, while raw query text and hit content stay available only to
`trusted_local` sinks. Blocking `memory_read_before` rewrite/veto remains
downstream because it changes recall semantics and needs a separate policy slice.

### Files Modified

- `include/oran/hook/payload.hpp`
- `src/oran-hook/bus.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `tests/hook/test_bus.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 180 snapshot, focused validation, and next-slice candidates.
- `docs/design-docs/permissions-and-hooks.md` — memory read payloads, redaction, and producer status.
- `docs/design-docs/memory-system.md` — shipped `memory.read.after` boundary and remaining read-before gap.
- `docs/design-docs/bootstrap-runtime.md` — runner read-after hook publishing for prompt-boundary and tool recall.
- `docs/product-specs/0005-memory-system.md` — lifecycle hook status and validation counts.
- `docs/product-specs/0015-blocking-hook-decisions.md` — clarified the blocking whitelist is unchanged.
- `docs/ARCHITECTURE.md` — hook/bootstrap/memory boundary inventory.
- `docs/QUALITY_SCORE.md` — hook/bootstrap/memory test counts and remaining gaps.
- `docs/releases/feature-release-notes.md` — user-visible memory read hook note.

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
  - `tests/hook/test_bus.cpp` covers default-vs-trusted memory read redaction.
  - `tests/bootstrap/test_prompt_runner.cpp` covers `memory_read_after`
    publishing for prompt-boundary recall and `memory.recall`.
- Bench impact: none; this is observability and redaction behavior, not a
  benchmark slice.
- Compile-budget delta: no new dependency or target-level compile-budget change.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`

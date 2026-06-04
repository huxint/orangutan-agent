## [2026-06-04 15:22] | Task: Add `DispatchContext::for_now`

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI in `/home/huxint/projects/orangutan-refactor`
- Linked plan: none — small deep-review tracker cleanup slice.

### User Query

> Deeply understand the project architecture and current progress, read the
> relevant documentation before further implementation, and start the next
> slice.

### Changes Overview

- Areas: `oran-tool` public API, scheduler/bootstrap dispatch context
  construction, focused tool tests, slice docs.
- Key actions: added `tool::DispatchContext::for_now(...)` overloads for fresh
  current-clock contexts and prototype clone/refresh, switched
  `agent::ToolScheduler` and `bootstrap::AgentPromptRunner` to consume them,
  and added direct factory coverage.

### Design Intent

The deep-review tracker kept `DispatchContext::for_now()` as a P2 cleanup item
because both direct runtime callers and concurrent scheduler calls need the same
"fresh dispatch context with a real clock" invariant. Keeping that as a public
factory prevents future callers from copying the full context field list while
still allowing tests to aggregate-initialise `DispatchContext` when they need a
pinned approval clock.

### Files Modified

- `include/oran/tool/registry.hpp`
- `src/oran-tool/registry.cpp`
- `src/oran-agent/scheduler.cpp`
- `include/oran/agent/scheduler.hpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/tool/test_registry.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/tool-runtime.md` — documents the public factory and the
  scheduler clone/refresh behavior.
- `docs/product-specs/0012-tool-scheduler-and-state.md` — notes that scheduler
  per-call context cloning now uses the public factory.
- `docs/ARCHITECTURE.md` — library inventory names the new `oran-tool` API.
- `docs/exec-plans/tech-debt-tracker.md` — removes the closed
  `DispatchContext::for_now()` deep-review item.
- `docs/STATUS.md` — slice/history pointer and focused test counts refreshed.
- `docs/QUALITY_SCORE.md` — tool/test rows refreshed.
- `docs/releases/feature-release-notes.md` — public API release note added.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake run test-tool` — 197 cases / 2049 assertions.
  - `xmake build test-agent`
  - `xmake run test-agent` — 56 cases / 10 744 assertions.
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap` — 101 cases / 711 assertions.
  - `xmake build orangutan`
  - `xmake run orangutan -- --help` — reports `orangutan v2.0.0-slice155`.
  - `make ci`
  - `xmake test` — 16 buckets passed.
- Tests added/changed: two direct `DispatchContext::for_now` tests covering
  fresh context creation and prototype clone/refresh.
- Bench impact: none; this removes duplicated setup and adds no competing
  runtime implementation.
- Compile-budget delta: no threshold changed; the public header gains only
  function declarations.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: the P2 `DispatchContext::for_now()` tracker item is
  closed; the remaining deep-review row still tracks larger future work such as
  `file.modify` / `directory.scan` and advisory hook parallel fan-out.
- Linked release note: 2026-06-04 `tool-dispatch-context-for-now`.

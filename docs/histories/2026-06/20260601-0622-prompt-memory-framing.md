## [2026-06-01 06:22] | Task: prompt memory framing

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI in `local repository checkout`
- Linked plan: `docs/exec-plans/completed/2026-06-01-memory-runtime-v1.md`

### User Query

Continue the doc-first memory-runtime plan and land the final narrow prompt
memory-slot follow-up after configured-route session persistence was stable.

### Changes Overview

- Areas: `oran-memory`, `oran-bootstrap`, memory/bootstrap tests, docs/status,
  release notes, completed exec-plan.
- Key actions:
  - Added `memory::Framing`, `memory::FramingStats`, and
    `memory::FramingOwner` as the minimal owner for prompt section 5.
  - Had `AgentPromptRunner` render memory framing once before entering
    `agent::Loop`, then pass the stable string through
    `RunTurnInputs::memory_framing`.
  - Exposed `AgentPromptRunner::memory_framing_renders()` for diagnostics and
    tests.
  - Added direct `test-memory` coverage for stable/empty/replaced framing and a
    bootstrap regression proving a multi-iteration turn renders framing once.
  - Moved the memory runtime v1 plan to completed and bumped the binary slice
    tag to `2.0.0-slice133`.

### Design Intent

Memory framing is a prompt-surface concern, but it should not live inside the
prompt builder or the ReAct loop. The builder already consumes stable section-5
bytes, and the loop may rebuild prompt bytes across provider/tool iterations.
Keeping `memory::FramingOwner` in `oran-memory` and rendering it once at the
runner boundary preserves the documented once-per-turn memory rule while giving
future long-term recall a single value to fill before loop entry.

### Files Modified

- `include/oran/memory/framing.hpp`
- `include/oran/memory.hpp`
- `src/oran-memory/framing.cpp`
- `include/oran/bootstrap/prompt_runner.hpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `tests/memory/test_framing.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — moved the project to slice 133, marked the memory plan
  complete, refreshed test counts, and set the next prompt-runtime slice.
- `docs/exec-plans/completed/2026-06-01-memory-runtime-v1.md` — recorded
  milestone 5, the decision to render framing outside `agent::Loop`, and the
  plan completion.
- `docs/QUALITY_SCORE.md` — updated `test-memory` / `test-bootstrap` counts and
  the bootstrap, prompt-builder, memory-tier, and agent notes.
- `docs/design-docs/memory-system.md` — documented `memory::FramingOwner` and
  the runner-side once-per-turn render boundary.
- `docs/design-docs/bootstrap-runtime.md` — documented the runner's memory
  framing render before loop entry.
- `docs/design-docs/agent-platform.md` — clarified that memory framing remains
  a stable loop input rendered by the runner.
- `docs/product-specs/0005-memory-system.md` — marked prompt memory framing as
  shipped for v1.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — updated the
  memory framing renderer status.
- `docs/ARCHITECTURE.md` — updated the `oran-memory` and `oran-agent` inventory
  rows.
- `docs/releases/feature-release-notes.md` — added the user-visible slice 133
  release note.

### Validation

- Commands run:
  - `xmake build test-memory`
  - `xmake build test-bootstrap`
  - `xmake run test-memory`
  - `xmake run test-bootstrap`
- Tests added/changed:
  - `test-memory` adds three framing-owner cases.
  - `test-bootstrap` adds a two-iteration runner case that asserts one memory
    framing render.
  - Focused results: `test-memory` 8 cases / 559 assertions;
    `test-bootstrap` 83 cases / 464 assertions.
- Bench impact:
  - No benchmark changed in this slice.
- Compile-budget delta:
  - No threshold changes; the new framing TU is small and keeps heavy session
    JSON serialization in `src/oran-memory/session.cpp`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`

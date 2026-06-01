## [2026-06-01 07:45] | Task: skill catalog renderer

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `CLI in /home/huxint/projects/orangutan-refactor`
- Linked plan: none

### User Query

> Continue the doc-first implementation plan for the current prompt-runtime slice.

### Changes Overview

- Areas: `oran-skill`, bootstrap prompt runner, prompt docs/build/test/bench docs.
- Key actions:
  - Added `skill::CatalogRenderer` and `skill::CatalogOwner`, plus `test-skill` / `bench-skill`.
  - Wired `AgentPromptRunner` to render section 4 once per prompt and expose a render counter.
  - Bumped the binary slice tag to `2.0.0-slice135` and synced status / release / quality docs.

### Design Intent

Keep skill bodies out of section 1 and out of the renderer. The slice intentionally
renders only a compact metadata snapshot; loader, watcher, and `skill.invoke` stay
downstream.

### Files Modified

- `include/oran/skill.hpp`
- `include/oran/skill/catalog.hpp`
- `src/oran-skill/catalog.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/skill/main.cpp`
- `tests/skill/test_catalog.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `bench/skill/main.cpp`
- `bench/skill/scenarios/catalog.cpp`
- `bench/skill/README.md`
- `xmake/targets.lua`
- `xmake/tests.lua`
- `xmake/bench.lua`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

List every doc edited in the same PR as part of this change. If the change
invalidated a doc and the matching edit is *missing*, the PR is incomplete.

- `docs/ARCHITECTURE.md` — `oran-skill` boundary and `oran-bootstrap` dependency updates.
- `docs/BUILD_SYSTEM.md` — `orangutan` target now links `oran-skill`.
- `docs/QUALITY_SCORE.md` — quality rows updated for slice 135.
- `docs/STATUS.md` — slice 135 snapshot and next-slice note.
- `docs/design-docs/agent-platform.md` — skill catalog renderer and owner status.
- `docs/product-specs/0009-skills.md` — slice 135 status and validation.
- `docs/product-specs/0010-benchmark-harness.md` — skill benchmark row.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — prompt-runtime slice status.
- `docs/releases/feature-release-notes.md` — user-visible slice summary.
- `docs/rules/prompt-design.md` — section-4 owner note.
- `docs/rules/testing-and-bench.md` — bench layout and skill coverage.
- `bench/README.md` — bench layout and live bucket note.
- `include/README.md` — library inventory.
- `tests/README.md` — live test bucket note.

If no docs needed editing, state explicitly: "No external doc invalidation; change is
internal to `<file>` and does not affect any documented contract."

### Validation

- Commands run: `xmake build test-skill`, `xmake run test-skill`, `xmake build bench-skill`, `xmake run bench-skill`, `xmake build test-bootstrap`, `xmake run test-bootstrap`, `scripts/check-deps.sh`
- Tests added/changed: `test-skill` 5 cases / 19 assertions; new bootstrap render-count test
- Bench impact: `bench-skill` order-trusting baseline vs. deterministic render
- Compile-budget delta: not separately measured; focused build targets stayed green

### Follow-ups

- Issues opened:
- Tech-debt entries:
- Linked release note:

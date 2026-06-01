## [2026-06-01 21:24] | Task: skill hot-reload

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local xmake / GCC 16.1 release targets
- Linked plan: none; `docs/STATUS.md` routes the prompt-runtime arc after slice 137.

### User Query

> Continue the prompt-runtime slice work with docs kept in sync and keep iterating from the current implementation progress.

### Changes Overview

- Areas: `oran-skill`, `oran-bootstrap`, prompt-runtime docs.
- Key actions:
  - Added `skill::WorkspaceSkillSnapshot`, a prompt-boundary owner for `<workspace>/.orangutan/skills/*.md` snapshots.
  - Refresh now reloads the compact section-4 catalog and the `skill.invoke` document vector together, so a turn uses one coherent snapshot.
  - Linux builds use inotify when available, while every refresh still compares a bounded content-aware directory signature so add/update/remove changes are visible before the next prompt even without an active watcher.
  - Bootstrap's `AgentPromptRunner` now refreshes that snapshot before each prompt when callers supplied `skills_directory`; exact `skills_catalog` bytes still bypass filesystem loading for tests and embedders.

### Design Intent

The slice closes skill hot-reload without moving skill body bytes into stable prompt sections. The catalog is allowed to change between prompts because section 4 is the skill-catalog section, while `skill.invoke` bodies still enter through ordinary conversation-tail tool results. The watcher is best-effort operational acceleration; the prompt-boundary signature check remains the correctness path and avoids prompt failure when inotify is unavailable.

### Files Modified

- `include/oran/skill/loader.hpp`
- `src/oran-skill/loader.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/skill/test_loader.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 138 snapshot, next-slice route, and focused validation.
- `docs/ARCHITECTURE.md` — `oran-skill`, `oran-prompt`, and `oran-bootstrap` inventory rows for snapshot refresh ownership.
- `docs/BUILD_SYSTEM.md` — `oran-skill` dependency note for watcher/signature refresh and `oran-io` cache invalidation.
- `docs/QUALITY_SCORE.md` — current skill/bootstrap/prompt rows and test bucket counts.
- `docs/design-docs/agent-platform.md` — section-4 prompt-boundary skill refresh and cache-prefix boundaries.
- `docs/design-docs/bootstrap-runtime.md` — runner-owned `WorkspaceSkillSnapshot` behavior.
- `docs/design-docs/memory-system.md` — once-per-turn memory framing plus prompt-boundary skill snapshot consistency.
- `docs/product-specs/0009-skills.md` — v1 hot-reload status and acceptance criteria.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — section-4 cache behavior for skill add/update/remove changes.
- `docs/rules/prompt-design.md` — section ownership remains stable for hot-reloaded skill snapshots.
- `docs/releases/feature-release-notes.md` — user-visible skill hot-reload note.
- `bench/skill/README.md`, `tests/README.md`, `include/oran/skill/loader.hpp` — bucket/surface notes for snapshot refresh.

### Validation

- Commands run:
  - `xmake build test-skill`
  - `xmake run test-skill`
  - `xmake build test-bootstrap`
  - `timeout 60s xmake run -y test-bootstrap`
  - `git diff --check`
  - `make ci`
  - `xmake run orangutan -- --help`
- Tests added/changed:
  - Added `test-skill` coverage for stable empty missing-directory snapshots, missing-directory creation, watcher-driven update/remove, and catalog/body snapshot replacement.
  - Added `test-bootstrap` coverage proving the runner refreshes the catalog before the next prompt and serves the updated body through `skill.invoke`.
- Bench impact: no new bench; watcher refresh is correctness-oriented and the signature path has no competing implementation in this slice.
- Compile-budget delta: no budget cap change; heavy filesystem/inotify includes stay in the private `oran-skill` implementation.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`.

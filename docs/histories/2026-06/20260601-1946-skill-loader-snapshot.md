## [2026-06-01 19:46] | Task: skill loader snapshot

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local xmake / GCC 16.1 release targets
- Linked plan: none; `docs/STATUS.md` routes the prompt-runtime arc after slice 135.

### User Query

> Continue iterating the execution plan after deeply understanding the project docs and current implementation progress.

### Changes Overview

- Areas: `oran-skill`, bootstrap prompt runner, prompt docs, build/dependency docs.
- Key actions:
  - Added `skill::Loader`, `SkillDocument`, and `SkillMetadata` for bounded markdown skill snapshots under `<workspace>/.orangutan/skills/*.md`.
  - Parsed single-line frontmatter fields (`name`, `description`, `triggers`, `inputs`, `model_hint`), rejected malformed delimiters/unknown fields/oversized bodies, and treated a missing skills directory as an empty snapshot.
  - Wired configured-route `AgentPromptRunner` to load the skills directory once before the first prompt unless exact `skills_catalog` bytes were supplied, preserving the once-per-turn section-4 render boundary.
  - Bumped the binary slice tag to `2.0.0-slice136` and documented the new downward `oran-skill -> oran-async/oran-io` dependency.

### Design Intent

This is the smallest useful skills-v1 increment after the section-4 owner: the runtime
can now snapshot existing local markdown skills into the compact catalog without
claiming hot reload or `skill.invoke`. Bodies are loaded and capped for the future
invoke path but are deliberately excluded from section 4 and section 1, matching
`docs/rules/prompt-design.md` and `docs/product-specs/0009-skills.md`.

### Files Modified

- `include/oran/skill/loader.hpp`
- `src/oran-skill/loader.cpp`
- `tests/skill/test_loader.cpp`
- `include/oran/bootstrap/prompt_runner.hpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `xmake/targets.lua`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 136 snapshot, next-slice route, focused validation.
- `docs/ARCHITECTURE.md` — `oran-skill` purpose/dependencies and bootstrap runner ownership.
- `docs/BUILD_SYSTEM.md` — `oran-skill` now depends on `oran-async` and `oran-io`.
- `docs/design-docs/module-boundaries.md` — dependency direction note updated for `skill -> async/io`.
- `docs/design-docs/bootstrap-runtime.md` — runner `skills_directory` snapshot behavior and diagnostics.
- `docs/design-docs/agent-platform.md` — section-4 loader snapshot status.
- `docs/product-specs/0009-skills.md` — loader status and watcher/invoke remaining scope.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — section-4 status and cache boundary.
- `docs/rules/prompt-design.md` — section-4 owner/loader status while invoke remains downstream.
- `docs/QUALITY_SCORE.md` — test counts and skills/bootstrap current-state rows.
- `docs/releases/feature-release-notes.md` — user-visible release note.
- `tests/README.md`, `bench/skill/README.md` — live skill bucket scope.

### Validation

- Commands run:
  - `xmake run test-skill` — 9 cases / 55 assertions.
  - `xmake run test-bootstrap` — 86 cases / 504 assertions. The sandboxed run failed in localhost provider-backend cases with `open: Operation not permitted`; rerun with normal socket permissions passed.
- Tests added/changed:
  - Added skill loader file/directory/catalog snapshot coverage, malformed metadata/body-cap rejection, missing-directory empty snapshot, and standalone frontmatter delimiter regression.
  - Added bootstrap prompt-runner coverage proving workspace skill snapshots appear in the prompt, exclude body text, render once, and stay byte-identical across loop iterations.
- Bench impact: no new bench; the loader is startup correctness work, while the existing `bench-skill` still covers deterministic catalog rendering.
- Compile-budget delta: no budget cap change; `oran-skill` adds small loader TUs and the existing category budget remains unchanged.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md#2026-06`.

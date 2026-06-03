## [2026-06-03 19:38] | Task: Session Skill Activations

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI in /home/huxint/projects/orangutan-refactor`
- Linked plan: none

### User Query

> Merge the previous slice to `main` first, then continue with the next adjacent
> implementation slice, commit it, and merge it back to `main`.

### Changes Overview

- Areas: `oran-storage`, `oran-memory`, `oran-skill`, `oran-bootstrap`, tests, docs.
- Key actions: added a sessions DB migration for `session_skill_activations`;
  exposed typed storage and memory upsert/load APIs; added
  `ActivationPolicy::session_skill_activations`; and had `AgentPromptRunner`
  load durable activation state before section-4 rendering, then persist
  successful `skill.invoke` / `skill.deactivate` updates after turn persistence.

### Design Intent

Slice 147 made activation/deactivation visible through transcript tool results, but
that source alone cannot survive transcript compaction or pruning. Slice 148 keeps
the transcript scan as the backward-compatible fallback while overlaying a durable
per-session `(session_id, agent_key, skill_name)` row that represents the latest
active/inactive decision. Config deactivation and expiration still subtract after
the overlay, prompt rendering remains prompt-boundary only, and the renderer still
does not read clocks or storage directly.

### Files Modified

- `include/oran/storage/session_repository.hpp`
- `include/oran/memory/session.hpp`
- `include/oran/skill/catalog.hpp`
- `src/oran-storage/migrations/sessions/0002-session-skill-activations.sql`
- `src/oran-storage/migration_assets.cpp`
- `src/oran-storage/session_repository.cpp`
- `src/oran-memory/session.cpp`
- `src/oran-skill/catalog.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `tests/storage/test_session_repository.cpp`
- `tests/memory/test_session_store.cpp`
- `tests/skill/test_catalog.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - moved the snapshot to slice 148 and recorded focused validation.
- `docs/product-specs/0009-skills.md` - documented durable session activation rows.
- `docs/design-docs/bootstrap-runtime.md` - documented runner load/persist policy flow.
- `docs/design-docs/memory-system.md` - documented `Store` skill activation APIs.
- `docs/design-docs/storage-runtime.md` - documented migration 2 and repository APIs.
- `docs/ARCHITECTURE.md` - updated storage, memory, skill, prompt, and bootstrap boundaries.
- `docs/QUALITY_SCORE.md` - updated counts and removed stale future-work wording.
- `docs/histories/2026-06/20260603-1938-session-skill-activations.md` - this entry.

### Validation

- Commands run:
  - `build/linux/x86_64/debug/test-bootstrap "AgentPromptRunner clears active markers after skill.deactivate"`
  - `xmake f -m release`
  - `xmake build test-skill`
  - `xmake run test-skill`
  - `xmake build test-storage`
  - `xmake run test-storage`
  - `xmake build test-memory`
  - `xmake run test-memory`
  - `xmake -r test-bootstrap`
  - `xmake run test-bootstrap`
  - `build/linux/x86_64/debug/test-bootstrap --reporter=console --verbosity=normal`
- Tests added/changed: storage upsert/load and validation; memory wrapper
  validation; skill policy overlay semantics; bootstrap persistence across
  transcript pruning and post-deactivation persistence.
- Bench impact: none; the new paths are correctness/persistence surfaces.
- Compile-budget delta: not measured; implementation stayed in existing storage,
  memory, skill, and bootstrap translation units.

Note: the release `xmake run test-bootstrap` passed 74 cases / 374 assertions but
did not register `tests/bootstrap/test_prompt_runner.cpp` cases in the rebuilt
release binary. The debug bootstrap binary registered the full 100 cases and passed
699 assertions, including the prompt-runner coverage changed in this slice.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: none.

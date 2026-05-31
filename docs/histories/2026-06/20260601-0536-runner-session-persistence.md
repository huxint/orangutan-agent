## [2026-06-01 05:36] | Task: runner session persistence

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI in `/home/huxint/projects/orangutan-refactor`
- Linked plan: `docs/exec-plans/active/2026-06-01-memory-runtime-v1.md`

### User Query

Continue the doc-first memory-runtime implementation plan, keep the active plan
current, and land the next coherent slice after the configured-route runner
could borrow the assembly-owned session store.

### Changes Overview

- Areas: `oran-bootstrap`, bootstrap tests, docs/status/history, release notes,
  memory-runtime docs.
- Key actions:
  - Wired `AgentPromptRunner` to load persisted session history from
    `RuntimeAssembly::session_store()` when available.
  - Appended only the new successful transcript suffix back to the session store
    after a turn completed successfully.
  - Kept the in-process transcript path as the fallback when session memory is
    disabled.
  - Added runner persistence coverage for first-run append and second-run reload
    behavior.
  - Bumped the binary slice tag to `2.0.0-slice132`.

### Design Intent

Configured-route prompts now need real conversation persistence, not just a
typed store and a bootstrap-owned database. This slice keeps the memory-layer
boundary intact by loading and appending at the runner boundary, while leaving
built-in no-route startup deterministic and in-process when session memory is
disabled. The design stays aligned with `docs/design-docs/memory-system.md` and
`docs/design-docs/bootstrap-runtime.md`.

### Files Modified

- `src/oran-bootstrap/prompt_runner.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

List every doc edited in the same PR as part of this change. If the change
invalidated a doc and the matching edit is *missing*, the PR is incomplete.

- `docs/STATUS.md` — moved the project to slice 132 and set the prompt-memory
  slot follow-up as the next intended slice.
- `docs/exec-plans/active/2026-06-01-memory-runtime-v1.md` — marked milestone 4
  complete and recorded the runner persistence slice.
- `docs/QUALITY_SCORE.md` — updated `test-bootstrap` coverage and the bootstrap
  / memory-tier status text.
- `docs/design-docs/bootstrap-runtime.md` — documented runner load/append
  behavior against the assembly-owned session store.
- `docs/design-docs/memory-system.md` — marked session persistence as live and
  moved the runner gap out of the design doc.
- `docs/product-specs/0005-memory-system.md` — marked bootstrap runner
  persistence shipped for the session store path.
- `docs/product-specs/0001-core-react-loop.md` — clarified that session history
  persistence is now part of the shipped configured-route bootstrap path.
- `docs/ARCHITECTURE.md` — updated the library inventory and bootstrap/agent
  ownership notes.
- `docs/releases/feature-release-notes.md` — added the user-visible slice 132
  release note.

### Validation

- Commands run:
  - `clang-format -i src/oran-bootstrap/prompt_runner.cpp tests/bootstrap/test_prompt_runner.cpp`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `scripts/check-deps.sh`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - `test-bootstrap` adds runner persistence coverage for append-on-success and
    reload-on-new-runner behavior.
  - Focused result: 82 cases / 452 assertions.
- Bench impact:
  - No benchmark changed in this slice.
- Compile-budget delta:
  - No threshold changes; `oran-bootstrap` already depended on the session
    memory owner from the previous slice.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`

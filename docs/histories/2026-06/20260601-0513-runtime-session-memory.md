## [2026-06-01 05:13] | Task: runtime session memory

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI in `local repository checkout`
- Linked plan: `docs/exec-plans/active/2026-06-01-memory-runtime-v1.md`

### User Query

Continue the doc-first memory-runtime implementation plan, keep the active plan
current, and land the next coherent slice after the typed session store.

### Changes Overview

- Areas: `oran-bootstrap`, `oran-memory` wiring, xmake targets, bootstrap tests,
  docs/status/history.
- Key actions:
  - Added `RuntimeAssemblyOptions::sessions_db_path`,
    `session_memory_enabled`, `session_reader_count`, and
    `session_statement_cache_capacity`.
  - Added `RuntimeAssembly::session_store()`,
    `session_memory_enabled()`, and `sessions_path()`.
  - Made runtime assembly migrate and own a separate
    `<workspace>/.orangutan/sessions.db` pool/repository/store over
    `memory::session::Store`.
  - Enabled session memory for configured provider routes and explicitly kept the
    built-in no-route CLI path session-memory disabled.
  - Extended the runtime assembly banner with session state/path.
  - Bumped the binary slice tag to `2.0.0-slice131`.

### Design Intent

This is milestone 3 of the memory runtime plan. Slice 130 gave the project a
typed session-memory API, but no process owner opened `sessions.db`. This slice
keeps session data separate from audit/trace evidence by giving
`RuntimeAssembly` a dedicated sessions pool and repository. It deliberately
does not persist `AgentPromptRunner` transcripts yet; the runner now has a stable
store to borrow, while no-route startup remains deterministic and avoids session
state on fresh checkouts.

### Files Modified

- `include/oran/bootstrap/runtime_assembly.hpp`
- `src/oran-bootstrap/runtime_assembly.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
- `tests/bootstrap/test_bootstrap.cpp`
- `xmake/targets.lua`
- Docs listed below.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

List every doc edited in the same PR as part of this change. If the change
invalidated a doc and the matching edit is *missing*, the PR is incomplete.

- `docs/STATUS.md` — moved the project to slice 131 and set runner
  persistence as the next intended memory slice.
- `docs/ARCHITECTURE.md` — updated the `oran-bootstrap` dependency surface and
  memory ownership notes.
- `docs/BUILD_SYSTEM.md` — updated the xmake target snippet and bootstrap/memory
  dependency description.
- `docs/QUALITY_SCORE.md` — recorded `test-bootstrap` 80 / 422 and the new
  runtime assembly memory status.
- `docs/design-docs/bootstrap-runtime.md` — documented the new session-memory
  runtime assembly API, banner, and no-route disablement.
- `docs/design-docs/memory-system.md` — moved session DB assembly from planned to
  implemented and left runner persistence as the next gap.
- `docs/design-docs/module-boundaries.md` — clarified the bootstrap composition
  root exception already enforced by `scripts/check-deps.sh`.
- `docs/product-specs/0005-memory-system.md` — marked bootstrap runtime
  ownership live for the session store path.
- `docs/exec-plans/active/2026-06-01-memory-runtime-v1.md` — marked milestone 3
  complete and linked this history/release artifact.
- `docs/releases/feature-release-notes.md` — added the user-visible release note.

### Validation

- Commands run:
- `clang-format -i include/oran/bootstrap/runtime_assembly.hpp src/oran-bootstrap/runtime_assembly.cpp src/oran-bootstrap/bootstrap.cpp tests/bootstrap/test_runtime_assembly.cpp tests/bootstrap/test_bootstrap.cpp`
- `xmake build oran-bootstrap`
- `xmake build test-bootstrap`
- `xmake run test-bootstrap`
- `xmake build orangutan`
- `xmake run orangutan -- --help`
- `scripts/check-deps.sh`
- `git diff --check`
- `make ci`
- Tests added/changed:
  - `test-bootstrap` adds runtime assembly coverage for default/explicit
    `sessions.db`, disabled session memory, store append/load through the
    assembly owner, and configured-route vs. no-route bootstrap behavior.
  - Focused result: 80 cases / 422 assertions.
- Bench impact:
  - No benchmark changed in this slice; runtime persistence is not wired yet.
- Compile-budget delta:
  - `oran-bootstrap` now depends on `oran-memory` at the composition-root layer;
    no compile-budget thresholds changed.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`

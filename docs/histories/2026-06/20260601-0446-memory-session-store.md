## [2026-06-01 04:46] | Task: memory session store

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI in `local repository checkout`
- Linked plan: `docs/exec-plans/active/2026-06-01-memory-runtime-v1.md`

### User Query

Continue the doc-first implementation plan, keep iterating the active execution
plan, and land the next coherent memory-runtime slice.

### Changes Overview

- Areas: `oran-memory`, xmake targets, tests, benches, docs/status/history.
- Key actions:
  - Added the first `oran-memory` library target and umbrella header.
  - Added `memory::session::Store` over `storage::SessionRepository`.
  - Kept message JSON serialization private to `src/oran-memory/session.cpp`.
  - Added `test-memory` coverage and `bench-memory` parity with the raw storage
    repository path.
  - Bumped the binary slice tag to `2.0.0-slice130`.

### Design Intent

This is milestone 2 of the memory runtime plan. The project already had the
storage foundation for session rows, but storage deliberately treats
`content_json` as opaque. This slice chooses the smallest useful memory owner:
`oran-memory::session::Store` serializes/deserializes `core::Message` at the
memory layer, preserving the documented `oran-storage` boundary while giving
bootstrap and the agent runner a typed API to wire in later slices. Runtime
assembly, configured-route persistence, long-term FTS5/vector search, memory
tools, and CLI session commands remain outside this slice.

### Files Modified

- `include/oran/memory.hpp`
- `include/oran/memory/session.hpp`
- `src/oran-memory/session.cpp`
- `tests/memory/main.cpp`
- `tests/memory/test_session_store.cpp`
- `bench/memory/README.md`
- `bench/memory/main.cpp`
- `bench/memory/scenarios/session_store.cpp`
- `xmake/targets.lua`
- `xmake/tests.lua`
- `xmake/bench.lua`
- `src/oran-bootstrap/bootstrap.cpp`
- Docs listed below.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — moved the project to slice 130 and set runtime assembly
  sessions DB as the next intended slice.
- `docs/ARCHITECTURE.md` — updated the library inventory for the new
  `oran-memory` target and dependency boundary.
- `docs/BUILD_SYSTEM.md` — registered the new target shape and private JSON use.
- `docs/QUALITY_SCORE.md` — added `test-memory` and `bench-memory` coverage to
  the current quality picture.
- `docs/design-docs/memory-system.md` — replaced the planned session-store API
  with the implemented wrapper shape and current status.
- `docs/product-specs/0005-memory-system.md` — marked the 500-message session
  acceptance criterion closed for the memory wrapper.
- `docs/exec-plans/active/2026-06-01-memory-runtime-v1.md` — marked milestone 2
  complete and linked this history/release artifact.
- `docs/rules/libraries.md` — documented `oran-memory` as a private
  `nlohmann_json` consumer while keeping SQLite behind `oran-storage`.
- `bench/README.md` and `tests/README.md` — marked the memory buckets live.
- `docs/releases/feature-release-notes.md` — added the user-visible release note.

### Validation

- Commands run:
  - `clang-format -i include/oran/memory.hpp include/oran/memory/session.hpp src/oran-memory/session.cpp tests/memory/main.cpp tests/memory/test_session_store.cpp bench/memory/main.cpp bench/memory/scenarios/session_store.cpp`
  - `xmake build oran-memory`
  - `xmake build test-memory`
  - `xmake run test-memory`
  - `xmake build bench-memory`
  - `timeout 120 xmake run bench-memory`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `scripts/check-deps.sh`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - `test-memory` added 5 cases / 550 assertions covering ordered round-trip,
    agent scoping, malformed stored JSON rejection, required-id validation, and
    the 500-message session round-trip criterion.
- Bench impact:
  - `memory.session_repository_append_load`: ~849.8 us / 64-message batch.
  - `memory.session_store_append_load`: ~1022.4 us / 64-message batch.
- Compile-budget delta:
  - New JSON-owning memory TU builds under the existing `oran-memory` compile
    budget category; no `compile_budget.json` threshold change.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`

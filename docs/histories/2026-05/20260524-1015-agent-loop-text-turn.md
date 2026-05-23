## [2026-05-24 10:15] | Task: agent loop text turn

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `/home/huxint/projects/orangutan-refactor`
- Linked plan: none — this is a narrow spec-0017 slice under the existing fake-provider-first sequencing contract.

### User Query

> Continue the project implementation after reading the current docs and state; keep one coherent version per commit, detailed code comments, and a standard detailed commit message.

### Changes Overview

- Areas: `oran-agent`, provider-loop wiring, build docs/status/history.
- Key actions: added the first `agent::Loop` public surface and implementation; wired `oran-agent` to `oran-async` and `oran-provider`; added focused loop tests; bumped the binary slice tag to `2.0.0-slice75`.

### Design Intent

Spec 0017 requires the first real agent loop to run against `provider::FakeProvider` before any vendor adapter. This slice opens that seam without pretending to be the complete ReAct loop: `Loop::run_turn` renders the prompt, maps cache hints and active/promoted tools into one `provider::Request` with a name-sorted native tool list, sends through a supplied `provider::System`, and returns terminal text turns. It explicitly rejects `tool_use` responses so the next slice can add tool dispatch, ordered `tool_result` appends, and provider re-entry without changing the provider contract or hiding an incomplete behavior behind a silent fallback.

### Files Modified

- `include/oran/agent/loop.hpp`
- `include/oran/agent.hpp`
- `src/oran-agent/loop.cpp`
- `tests/agent/test_loop.cpp`
- `xmake/targets.lua`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/BUILD_SYSTEM.md`
- `docs/design-docs/agent-platform.md`
- `docs/product-specs/0017-fake-provider-first-agent-loop.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 75, pointed at this history, and named the next tool-dispatch loop slice.
- `docs/ARCHITECTURE.md` — documented the new `agent::Loop` surface and live `oran-agent -> oran-provider` dependency.
- `docs/BUILD_SYSTEM.md` — updated the target snippet and explained the new downward provider dependency.
- `docs/design-docs/agent-platform.md` — refreshed prompt/loop status from provider prework to text-turn loop foundation.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — recorded which subset of the MVP loop now exists and which scenarios remain.
- `docs/QUALITY_SCORE.md` — updated `test-agent` counts and the agent-runtime row.
- `docs/releases/feature-release-notes.md` — added the user-visible slice-75 release note.

### Validation

- Commands run:
  - `xmake build test-agent`
  - `xmake run test-agent`
  - `scripts/check-deps.sh`
  - `scripts/check-includes.sh`
- Tests added/changed: `tests/agent/test_loop.cpp` adds four loop cases; `test-agent` now reports 7 cases / 64 assertions.
- Bench impact: no new bench; loop-overhead comparison waits for a realistic multi-iteration workload.
- Compile-budget delta: not measured in this slice; public headers passed include hygiene and the new implementation is one small `oran-agent` TU.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` row `agent-loop-text-turn`.

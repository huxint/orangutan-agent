## [2026-06-05 11:27] | Task: Configured long-term recall policy

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none

### User Query

> Start the next slice after re-orienting on project architecture and current implementation progress; finish with docs, verification, and a commit.

### Changes Overview

- Areas: `oran-config`, `oran-bootstrap`, long-term memory prompt recall.
- Key actions: added typed `memory.longterm.recall.enabled` / `limit` config, mapped it into `AgentPromptRunnerOptions::longterm_recall` for configured-route startup, documented the new production policy source, and bumped the binary slice tag to `2.0.0-slice165`.

### Design Intent

Slice 164 deliberately kept long-term recall as an explicit runner option. This slice keeps the same prompt-boundary contract and query derivation, then adds only the conservative production policy source: config can opt in and choose a positive result limit, while defaults leave configured-route prompts unchanged. Richer query/ranking policy, vector composition, and memory tools stay downstream.

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/config/test_config.cpp`
- `tests/bootstrap/test_bootstrap.cpp`
- `config.example.json`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — config, memory, and bootstrap inventory rows now name the slice-165 mapping.
- `docs/design-docs/bootstrap-runtime.md` — documents the configured-route policy source.
- `docs/design-docs/memory-system.md` — records config-driven recall enablement and remaining memory work.
- `docs/design-docs/secrets-and-state.md` — documents the typed memory config block and fixes the provider credential-boundary status.
- `docs/product-specs/0005-memory-system.md` — updates long-term recall status and focused validation counts.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — updates section-5 recall status.
- `docs/QUALITY_SCORE.md` — updates config/bootstrap counts and remaining memory follow-ups.
- `docs/STATUS.md` — bumps slice, history pointer, current counts, and open-debt summary.
- `docs/exec-plans/tech-debt-tracker.md` — removes the configured-route recall policy mapping from the P3 memory follow-up.
- `docs/releases/feature-release-notes.md` — adds the operator-visible config behavior.

### Validation

- Commands run: `xmake build test-config`, `xmake build test-bootstrap`, `xmake run test-config`, `xmake run test-bootstrap`, `make ci`, `xmake -j$(nproc)`, `xmake test`.
- Tests added/changed: config parser coverage for defaults, valid recall policy, malformed recall policy, and strict/loose unknown nested memory fields; bootstrap coverage for a configured-route prompt that recalls seeded long-term memory into the provider request.
- Bench impact: none; this is startup/config mapping and prompt-boundary behavior, not a measured hot path.
- Compile-budget delta: no heavy public includes added; focused test buckets rebuilt.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: gated sqlite-vec/vector composition, richer recall query policy, hybrid ranking, and memory tools remain in the tracker.
- Linked release note: `docs/releases/feature-release-notes.md#2026-06`

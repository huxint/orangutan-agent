## [2026-05-24 12:25] | Task: trace config policy

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `/home/huxint/projects/orangutan-refactor`
- Linked plan: none — focused spec-0018 config-policy slice after the first terminal trace writer.

### User Query

> Continue the project implementation after reading the current docs and state; keep one coherent version per commit, detailed code comments, and a standard detailed commit message.

### Changes Overview

- Areas: `oran-config`, config example, spec-0018 trace policy.
- Key actions: added `config::TraceConfig`, exposed `Config::trace()`, parsed top-level `trace.enabled`, `trace.store_raw_bodies`, and `trace.retention_days`, documented the default block in `config.example.json`, and bumped the binary slice tag to `2.0.0-slice81`.

### Design Intent

The trace row writer from slice 80 is still caller-configured through `RunTurnInputs::trace`. This slice turns the spec-0018 operator policy into typed config without prematurely wiring runtime behavior: `enabled=false`, raw-body storage, and retention enforcement need bootstrap/loop ownership decisions, so the config loader only validates and exposes the policy. The top-level JSON shape matches the spec verbatim and keeps defaults conservative: tracing enabled, raw bodies disabled, retention set to 30 days.

### Files Modified

- `config.example.json`
- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/config/test_config.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/secrets-and-state.md`
- `docs/product-specs/0018-first-loop-observability.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 81, pointed at this history, and recorded the new typed trace policy plus the remaining bootstrap/loop wiring.
- `docs/ARCHITECTURE.md` — added trace to the `oran-config` inventory row.
- `docs/design-docs/secrets-and-state.md` — documented the new top-level typed config block.
- `docs/product-specs/0018-first-loop-observability.md` — marked the operator config shape as parsed but not yet consumed.
- `docs/QUALITY_SCORE.md` — refreshed `test-config` counts and the config/storage/bootstrap rows.
- `docs/releases/feature-release-notes.md` — added the slice-81 release note.

### Validation

- Commands run:
  - `xmake run test-config`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `make ci`
  - `git diff --check`
- Tests added/changed: `tests/config/test_config.cpp` covers explicit trace policy values, malformed trace policy, and the updated checked-in example. Focused count: `test-config` 30 cases / 225 assertions.
- Bench impact: no new benchmark; this slice adds a small config parse branch and no runtime hot path.
- Compile-budget delta: not measured in this slice; the public header adds a tiny value type only.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` row `trace-config-policy`.

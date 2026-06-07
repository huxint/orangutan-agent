## [2026-06-07 23:56] | Task: Automation cron config seeds

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most valuable current slice in
> `orangutan-refactor` with docs-first workflow, focused validation, and a
> Conventional Commit. Also avoid leaving completed active docs unarchived.

### Changes Overview

- Areas: `oran-config`, `oran-bootstrap`, automation cron category docs.
- Key actions:
  - Added typed `automation.cron.jobs[]` parsing with UTC timestamp handling,
    unique non-empty job keys, optional `last_fired_at`, and strict/loose
    unknown-field behavior.
  - Added `bootstrap::cron_jobs_from(...)`, which validates cron expressions
    through `oran-automation` and maps config rows into
    `automation::UpsertCronJobRequest` descriptors.
  - Mapped and validated cron seeds during bootstrap config assembly even when
    no provider route is configured.
  - Stored mapped cron descriptors on `RuntimeAssemblyOptions::cron_jobs` /
    `RuntimeAssembly::cron_jobs()` without opening `automation.db` or starting
    timers.
  - Updated the startup banner with `automation-cron-jobs=<count>`.
  - Bumped the binary slice tag to `2.0.0-slice203`.
  - Archived the completed automation retention cadence exec plan.

### Design Intent

Cron config ownership is now a typed seed boundary, not a scheduler. Config owns
the authored JSON shape and UTC timestamps; bootstrap is the only layer that can
validate cron expressions against `oran-automation` without violating the
dependency direction. The mapped descriptors are intentionally not persisted or
run by `RuntimeAssembly::build(...)`: a future explicit runtime owner must
decide when to upsert them into `automation.db` and how to start any process
service loop.

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `tests/config/test_config.cpp`
- `include/oran/bootstrap.hpp`
- `include/oran/bootstrap/automation_cron.hpp`
- `src/oran-bootstrap/automation_cron.cpp`
- `include/oran/bootstrap/runtime_assembly.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-bootstrap/runtime_assembly.cpp`
- `tests/bootstrap/test_memory_retention.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
- `config.example.json`
- `docs/STATUS.md`
- `docs/QUALITY_SCORE.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/automation-runtime.md`
- `docs/design-docs/secrets-and-state.md`
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md`
- `docs/exec-plans/completed/2026-06-07-automation-retention-cadence.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/automation-runtime.md` — documents cron config seed
  semantics, bootstrap mapping, and assembly storage.
- `docs/design-docs/secrets-and-state.md` — updates the config model and strict
  unknown-field description for `automation.cron.jobs[]`.
- `docs/product-specs/0006-automation.md` — records shipped cron config seeds
  and keeps persistence/service startup downstream.
- `docs/ARCHITECTURE.md` — updates config/bootstrap/automation responsibilities.
- `docs/QUALITY_SCORE.md` — updates config/bootstrap counts and slice coverage.
- `docs/STATUS.md` — bumps slice/history/result/next boundary.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — records the
  config-seed milestone and next boundary.
- `docs/exec-plans/completed/2026-06-07-automation-retention-cadence.md` —
  archives the completed retention cadence plan.
- `docs/releases/feature-release-notes.md` — adds the user-facing release note.
- `config.example.json` — shows a valid `automation.cron.jobs[]` seed.

### Validation

- Commands run:
  - `xmake build test-config`
  - `xmake run test-config` — 51 cases / 458 assertions
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap` — 128 cases / 1077 assertions
- Tests added/changed:
  - Added config parser coverage for valid/default/malformed cron seeds.
  - Added bootstrap mapping coverage for valid seeds and invalid cron
    expressions.
  - Added bootstrap startup coverage that rejects invalid cron expressions even
    without a configured provider route.
  - Added runtime assembly coverage proving cron seeds are stored without
    creating `automation.db`.
- Bench impact: not benchmark-relevant; no scheduler tick loop or hot path
  policy changed.
- Compile-budget delta: one small `oran-bootstrap` translation unit plus a
  lightweight public helper header; no new target or third-party dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`

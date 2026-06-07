## [2026-06-08 00:31] | Task: Automation cron seed apply

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue implementing the most valuable current slice in `orangutan-refactor`
> with docs-first workflow, focused validation, and a Conventional Commit.

### Changes Overview

- Areas: `oran-automation`, `oran-bootstrap` tests, automation cron docs.
- Key actions:
  - Added `CronSeedApplyResult` and
    `AutomationRuntime::apply_cron_job_seeds(...)`.
  - The runtime helper takes mapped `UpsertCronJobRequest` rows, upserts them
    through the caller-owned repository, returns requested/upserted counts plus
    stored records, and annotates failures with `seed_index` / `job_key`.
  - Added automation runtime coverage for apply, update, and failure context.
  - Added bootstrap/runtime-assembly coverage proving `RuntimeAssembly` only
    stores descriptors and a caller must explicitly open `AutomationRuntime`
    before applying seeds.
  - Bumped the binary slice tag to `2.0.0-slice204`.

### Design Intent

Cron seed persistence is now explicit runtime setup, not bootstrap startup
ownership. This gives embedders a direct path from `RuntimeAssembly::cron_jobs()`
to caller-owned `automation.db` state while preserving the rule that bootstrap
does not create automation state, start timers, enqueue work, notify channels,
or call agents.

The helper applies seeds sequentially and stops on the first failure. Already
upserted rows remain committed; transactional all-or-nothing seed application
stays downstream unless a later runtime owner needs it.

### Files Modified

- `include/oran/automation/runtime.hpp`
- `include/oran/automation.hpp`
- `src/oran-automation/runtime.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_runtime.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
- `docs/STATUS.md`
- `docs/QUALITY_SCORE.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/automation-runtime.md`
- `docs/product-specs/0006-automation.md`
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/automation-runtime.md` — documents explicit cron seed
  application semantics, API shape, non-transactional failure behavior, and
  validation counts.
- `docs/product-specs/0006-automation.md` — records slice 204 and moves cron
  seed application from open work to the explicit runtime boundary.
- `docs/ARCHITECTURE.md` — updates automation ownership notes and remaining
  scheduler gaps.
- `docs/QUALITY_SCORE.md` — refreshes automation/bootstrap test counts and next
  target.
- `docs/STATUS.md` — bumps the project snapshot to slice 204.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — records the
  progress and decision log entry.
- `docs/releases/feature-release-notes.md` — adds the user-facing release note.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-automation "AutomationRuntime applies cron job seeds explicitly"`
  - `build/linux/x86_64/release/test-bootstrap "RuntimeAssembly cron seeds persist only through caller-owned automation runtime"`
  - `xmake run test-automation` — 57 cases / 747 assertions
  - `xmake run test-bootstrap` — 129 cases / 1087 assertions
- Tests added/changed:
  - Added automation runtime coverage for explicit seed apply/update/failure
    context.
  - Added bootstrap runtime-assembly coverage for explicit assembly-to-runtime
    seed persistence.
- Bench impact: not benchmark-relevant; no scheduler loop hot path changed.
- Compile-budget delta: small method body in existing `runtime.cpp`; no new
  target or dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`

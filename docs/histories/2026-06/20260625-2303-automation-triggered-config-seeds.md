## [2026-06-25 23:03] | Task: automation triggered config seeds

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI in `/home/huxint/projects/orangutan-refactor`
- Linked plan: none; this is the next automation slice selected from
  `STATUS.md` / `ROADMAP.md` after the slice-256 channel ingress frontier.

### User Query

> 继续推进

### Changes Overview

- Areas: `oran-config`, `oran-bootstrap`, `oran-automation`, `--serve`, docs/status.
- Key actions:
  - Added typed `automation.triggered.jobs[]` config parsing with strict validation.
  - Added bootstrap mapping from triggered config rows to
    `automation::UpsertTriggeredJobRequest`.
  - Added `RuntimeAssemblyOptions::triggered_jobs` and
    `RuntimeAssembly::triggered_jobs()` diagnostics storage without opening
    `automation.db`.
  - Added explicit `AutomationRuntime::apply_triggered_job_seeds(...)` persistence.
  - Updated `--serve` to enable automation when either cron or triggered jobs are
    configured, apply cron seeds then triggered seeds once, and report both counts.

### Design Intent

The previous runtime could enqueue triggered work and persist triggered descriptors,
but operators had no config-authored path to seed those descriptors. This slice closes
that descriptor handoff before adding a producer: config now describes durable
triggered jobs, bootstrap maps and stores them, and explicit runtime owners can persist
them. `--serve` applies the seeds because it is already the long-lived automation owner.
It still does not fabricate trigger events; a future channel/webhook ingress must call
`AutomationService::enqueue_triggered(...)`.

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `include/oran/bootstrap/automation_cron.hpp`
- `src/oran-bootstrap/automation_cron.cpp`
- `include/oran/bootstrap/runtime_assembly.hpp`
- `src/oran-bootstrap/runtime_assembly.cpp`
- `include/oran/bootstrap/serve.hpp`
- `src/oran-bootstrap/serve.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `include/oran/automation/runtime.hpp`
- `src/oran-automation/runtime.cpp`
- `config.example.json`
- `tests/config/test_config.cpp`
- `tests/bootstrap/test_memory_retention.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`
- `tests/bootstrap/test_serve.cpp`
- `tests/automation/test_runtime.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/automation-runtime.md` — slice 257 narrative and public
  cron/triggered seed surface.
- `docs/design-docs/bootstrap-runtime.md` — `RuntimeAssembly` seed storage and
  `--serve` cron/triggered seed application.
- `docs/product-specs/0006-automation.md` — shipped prework, current API,
  acceptance status, and test counts.
- `docs/product-specs/index.md` — automation status now reflects `--serve` seed
  ownership plus pending triggered producer.
- `docs/ARCHITECTURE.md` — config/bootstrap/automation ownership notes for
  triggered seeds.
- `docs/QUALITY_SCORE.md` — automation/test surface and counts.
- `docs/ROADMAP.md` — automation frontier moved to slice 257.
- `docs/STATUS.md` — current slice, history pointer, latest slice, next slice, and
  library counts.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build orangutan`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-config`
  - `build/linux/x86_64/release/test-automation`
  - `xmake run test-bootstrap`
  - `xmake run orangutan -- --help`
  - `build/linux/x86_64/release/test-config "[automation]"`
  - `build/linux/x86_64/release/test-automation "[runtime][triggered]"`
  - `build/linux/x86_64/release/test-bootstrap "[automation]"`
  - `build/linux/x86_64/release/test-bootstrap "[serve][automation]"`
  - `xmake test`
  - `make ci`
- Tests added/changed:
  - Config parse/default/malformed coverage for `automation.triggered.jobs[]`.
  - Bootstrap mapper and `RuntimeAssembly` storage coverage for triggered seeds.
  - Runtime explicit triggered seed application coverage, including update and
    error context.
  - `--serve` automation coverage for queued triggered work with cron idle.
- Bench impact: no benchmark change; this is startup/config and repository seed
  handoff work.
- Compile-budget delta: not measured; affected code is descriptor parsing,
  bootstrap mapping, and runtime seed application.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`

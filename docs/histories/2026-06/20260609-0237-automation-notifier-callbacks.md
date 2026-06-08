## [2026-06-09 02:37] | Task: automation notifier callbacks

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local CLI`
- Linked plan: `docs/exec-plans/active/2026-06-07-automation-cron-category.md`

### User Query

> Continue the highest-value current slice in `orangutan-refactor`, follow the
> full repo workflow, and keep shipping slices end to end instead of stopping
> at partial code or STATUS churn.

### Changes Overview

- Areas: `oran-automation`, automation docs/spec/plan/status tracking.
- Key actions: added caller-owned notifier callbacks for cron and triggered
  durable execution, preserved optional handler output text on successful
  automation attempts, passed the same notifier surface through
  `TriggeredQueue`, and added focused coverage proving cron notifications run
  only after durable state is visible while notifier failures remain advisory
  for triggered success.

### Design Intent

Slice 221 closed the gap between durable automation prompts and the real
configured-route agent runtime, but runtime owners still had no small boundary
for observing durable job outcomes without re-reading repository state or
choosing a concrete channel-delivery stack. Slice 222 adds exactly that seam:
one post-outcome callback after durable cron/triggered state has already been
recorded. This keeps notification ownership caller-owned and advisory. The
automation library reports what happened, including optional handler text, but
does not decide how to render or deliver that output to CLI, channel, or
desktop surfaces. That concrete routing remains a later slice.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/queue.hpp`
- `include/oran/automation/service.hpp`
- `src/oran-automation/prompt.cpp`
- `src/oran-automation/queue.cpp`
- `src/oran-automation/runtime.cpp`
- `src/oran-automation/service.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_service.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumps the snapshot to slice 222 and records the completed
  notifier-callback boundary.
- `docs/ARCHITECTURE.md` — updates the automation ownership notes and library
  inventory to reflect caller-owned notifier callbacks and output-carrying
  attempt results.
- `docs/design-docs/automation-runtime.md` — documents the notifier types,
  service/queue option pass-through, durable post-outcome timing, and advisory
  failure semantics.
- `docs/product-specs/0006-automation.md` — updates shipped prework, current
  implementation, acceptance status, and still-open downstream routing work.
- `docs/QUALITY_SCORE.md` — updates `test-automation` counts and the automation
  row summary/next step.
- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` — records
  slice-222 progress, validation targets, and the design rationale for adding
  callbacks before concrete routing.
- `docs/releases/feature-release-notes.md` — adds the user-facing release note
  for notifier callbacks.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `xmake run test-automation`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `git diff --check`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed: added cron notifier success coverage that proves the
  callback only runs after the durable run row and `last_fired_at` advancement
  are visible, plus triggered notifier failure coverage that proves durable
  success is preserved and the notifier failure is surfaced only as advisory
  attempt metadata.
- Bench impact: none; the slice adds execution-surface reporting and focused
  tests only.
- Compile-budget delta: no new public heavy includes and no new library
  dependency direction; changes stay inside the existing automation service,
  runtime, and queue translation units.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`

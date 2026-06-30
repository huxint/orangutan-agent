## [2026-07-01 02:08] | Task: automation webhook payload producer

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI workspace
- Linked plan: none; this is a small Automation-track slice under the ROADMAP
  frontier.

### User Query

> Continue implementing the project, pick the most valuable next step, keep
> progress balanced across modules, avoid redundancy, and keep the structure
> clean.

### Changes Overview

- Areas: `oran-automation`, bootstrap automation prompt bridge, docs/status.
- Key actions:
  - Added optional triggered-event payload propagation from intake/enqueue
    through queued execution, handler calls, notifier payloads, and
    `AutomationPromptRunRequest`.
  - Added `automation::webhook_trigger_key(...)` plus `WebhookProducer`, a thin
    caller-owned non-chat producer seam that maps webhook ids to
    `webhook:<id>` trigger keys and enqueues through `AutomationService`.
  - Taught the bootstrap automation prompt bridge to append non-empty trigger
    payloads to the single-shot prompt under a stable `Trigger payload:` label.
  - Bumped the binary slice tag to `2.0.0-slice268`.

### Design Intent

The ROADMAP pointed the next balanced slice at Automation webhook/non-chat
producers. The repository does not yet have an HTTP server surface in
`oran-http`, so this slice deliberately stops at the automation boundary rather
than introducing a listener and config block at the same time. Webhook handling
now has a reusable producer and payload channel; a later interface slice can
bind an HTTP route to this seam without changing queue, execution, or prompt
contracts again. Channel-triggered jobs keep using the same enqueue path with
no payload.

### Files Modified

- `include/oran/automation.hpp`
- `include/oran/automation/prompt.hpp`
- `include/oran/automation/queue.hpp`
- `include/oran/automation/service.hpp`
- `include/oran/automation/webhook.hpp`
- `include/oran/bootstrap/automation_prompt_runner.hpp`
- `src/oran-automation/prompt.cpp`
- `src/oran-automation/queue.cpp`
- `src/oran-automation/service.cpp`
- `src/oran-automation/webhook.cpp`
- `src/oran-bootstrap/automation_prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/automation/test_queue.cpp`
- `tests/automation/test_service.cpp`
- `tests/bootstrap/test_automation_prompt_runner.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/automation-runtime.md` - documents webhook producer seam
  and triggered payload propagation.
- `docs/design-docs/bootstrap-runtime.md` - updates `--serve` follow-up wording
  now that the automation-side webhook seam exists.
- `docs/product-specs/0006-automation.md` - updates shipped automation status
  and remaining webhook HTTP listener follow-up.
- `docs/rules/prompt-design.md` - routes automation prompt payload bytes to the
  Automation spec/design docs.
- `docs/ARCHITECTURE.md` - updates the `oran-automation` inventory summary.
- `docs/ROADMAP.md` - moves the Automation frontier to slice 268.
- `docs/QUALITY_SCORE.md` - refreshes automation/test counts and next step.
- `docs/STATUS.md` - bumps current slice and latest history.
- `docs/releases/feature-release-notes.md` - adds the user-visible slice note.

### Validation

- Commands run:
  - `xmake build test-automation`
  - `xmake run test-automation "WebhookProducer normalizes webhook triggers and preserves payload" "TriggeredQueue enqueues matched triggered jobs for explicit receive" "Triggered prompt handler runs stored triggered job prompt"` (Catch2 ran the full bucket: 108 cases / 1886 assertions)
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap "automation prompt bridge drives triggered execution through TriggeredService"` (Catch2 ran the full bucket: 184 cases / 1799 assertions)
  - `xmake build orangutan && xmake run orangutan -- --help | head -1` (reported `orangutan v2.0.0-slice268`)
  - `xmake test` (19/19 buckets passed)
  - `make ci`
- Tests added/changed: added webhook producer payload coverage in
  `test-automation`; extended triggered queue/service and bootstrap prompt
  bridge payload assertions.
- Bench impact: none; this is a cold producer/queue handoff and prompt assembly
  path, not a hot scheduling loop.
- Compile-budget delta: one small `oran-automation` TU; no measured budget run.
  Full release build/test stayed green locally.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none; the HTTP listener/config slice remains the ROADMAP
  next step.
- Linked release note: `docs/releases/feature-release-notes.md` row
  `automation-webhook-payload-producer`.

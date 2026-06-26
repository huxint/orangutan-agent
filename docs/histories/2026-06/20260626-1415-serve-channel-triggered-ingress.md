## [2026-06-26 14:15] | Task: serve channel triggered ingress

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local CLI, xmake release build`
- Linked plan: none - the work fits the `STATUS.md` next-slice line.

### User Query

> Continue advancing the project implementation.

### Changes Overview

- Areas: `oran-bootstrap` service mode, channel ingress, automation triggered work.
- Key actions:
  - Added an optional `AutomationService*` hook to `bootstrap::serve_channels`.
  - When `--serve` owns both channel ingress and automation, channel messages now
    enqueue triggered automation work with trigger key `channel:<channel_id>`
    before the direct channel reply path runs.
  - Kept enqueue errors report-and-continue so a transient automation queue or DB
    issue does not block the user-visible channel reply.
  - Updated `config.example.json` to demonstrate `channel:mock-main`.
  - Added a bootstrap regression proving mock channel ingress still replies and
    also leaves one matching triggered job queued.

### Design Intent

Slice 257 made triggered descriptors durable but left them without a producer.
The smallest useful producer already exists at the process-owner boundary:
`orangutan --serve` has both the channel fan-in loop and the composed
`AutomationService` in scope. This slice therefore wires channel events there
instead of adding hidden ownership to `oran-channel` or `oran-automation`.

The trigger key convention is deliberately simple: `channel:<channel_id>`.
It avoids a new config field while giving operators a deterministic subscription
key for every configured channel. The direct reply path is unchanged; a channel
message can both receive an immediate response and enqueue background automation.
Webhook/non-chat producers and concrete notifier routing remain separate slices.

### Files Modified

- `include/oran/bootstrap/serve.hpp`
- `src/oran-bootstrap/serve.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_serve.cpp`
- `config.example.json`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 258 snapshot, history pointer, counts, and next step.
- `docs/ROADMAP.md` - Automation frontier and dependency frontier updated.
- `docs/ARCHITECTURE.md` - bootstrap/automation service boundary updated.
- `docs/design-docs/automation-runtime.md` - slice 258 producer note.
- `docs/design-docs/bootstrap-runtime.md` - `--serve` automation/channel owner updated.
- `docs/design-docs/channel-abstraction.md` - channel owner now notes automation enqueue.
- `docs/product-specs/0003-multi-platform-channels.md` - shipped channel producer note.
- `docs/product-specs/0006-automation.md` - AC3 status updated.
- `docs/QUALITY_SCORE.md` - bootstrap test count and Automation row updated.
- `docs/releases/feature-release-notes.md` - slice 258 release note.
- `config.example.json` - example trigger key changed to `channel:mock-main`.

### Validation

- Commands run:
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap "serve_channels enqueues matching automation triggers before replying" --reporter compact --success`
  - `xmake test`
  - `make ci`
- Tests added/changed:
  - `tests/bootstrap/test_serve.cpp` adds
    `serve_channels enqueues matching automation triggers before replying`.
- Bench impact: none; this is a cold service-owner enqueue path, not a hot loop.
- Compile-budget delta: not measured; the change adds no new heavy public include.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md` row `serve-channel-triggered-ingress`.

## [2026-07-01 12:27] | Task: automation webhook HTTP listener

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI
- Linked plan: none — small slice under the Automation roadmap row.

### User Query

> Continue implementing the project, keep the architecture balanced, and first fix
> the previous commit message format.

### Changes Overview

- Areas: `oran-config`, `oran-bootstrap`, automation docs/status.
- Key actions: fixed the previous commit subject to Conventional Commit format;
  added typed `automation.webhooks.listener` config with strict-mode validation;
  added the `serve_webhooks(...)` HTTP intake concern under `--serve`; wired an
  enabled listener to the existing `AutomationService`/`WebhookProducer` path;
  added config and loopback intake coverage.

### Design Intent

Slice 268 created the automation-owned `WebhookProducer` seam but deliberately
left HTTP ownership outside `oran-automation`. This slice puts the first listener
at the bootstrap service boundary, where `--serve` already owns long-lived
process concerns and the composed automation service. The listener is intentionally
narrow: `POST <path_prefix><id>` with `Content-Length` only, no generic router,
and no chunked decoding. A reusable HTTP server abstraction can be extracted once
a second runtime surface needs it.

### Files Modified

- `include/oran/config/config.hpp`
- `include/oran/automation.hpp`
- `include/oran/automation/webhook.hpp`
- `src/oran-config/config.cpp`
- `include/oran/bootstrap/serve.hpp`
- `src/oran-bootstrap/serve.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/config/test_config.cpp`
- `tests/bootstrap/test_serve.cpp`
- `config.example.json`
- `docs/histories/2026-07/20260701-1227-automation-webhook-http-listener.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/automation-runtime.md` — records the listener/config binding.
- `docs/design-docs/bootstrap-runtime.md` — records the new config fields and
  `--serve` startup surface.
- `docs/product-specs/0006-automation.md` — moves webhook listener binding out
  of the open list.
- `docs/ARCHITECTURE.md` — refreshes the automation/bootstrap boundary note.
- `docs/ROADMAP.md` — advances the Automation row frontier/next step.
- `docs/STATUS.md` — bumps to slice 269 and refreshes validation counts.
- `docs/QUALITY_SCORE.md` — refreshes automation/config/bootstrap coverage notes.
- `docs/releases/feature-release-notes.md` — adds the user-visible slice note.

### Validation

- Commands run:
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap "[webhook]"` (requires loopback
    socket permission)
  - `build/linux/x86_64/release/test-bootstrap` (requires loopback socket
    permission; 186 cases / 1824 assertions)
  - `xmake build test-config && xmake run test-config "*automation webhook listener*"`
- Tests added/changed: config parser coverage for `automation.webhooks.listener`
  plus loopback `serve_webhooks` POST-to-triggered-payload and open-connection
  stop/drain tests.
- Bench impact: none; listener startup/intake is not a hot path.
- Compile-budget delta: not measured separately; change stays in existing
  config/bootstrap TUs.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`.

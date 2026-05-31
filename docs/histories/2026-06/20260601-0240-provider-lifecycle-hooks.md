## [2026-06-01 02:40] | Task: slice 126 — provider lifecycle hooks

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local shell, xmake release`
- Linked plan: none — single-slice follow-up selected from `docs/STATUS.md`

### User Query

Continue the doc-first architecture/progress pass, keep the execution plan moving, and
implement the next coherent runtime slice only after reading the relevant docs.

### Changes Overview

- Areas: `oran-hook`, `oran-agent`, `oran-bootstrap`, provider lifecycle docs.
- Key actions:
  - Added metadata-only provider lifecycle payloads to `oran-hook`.
  - Published advisory `provider_request`, `provider_response`,
    `provider_error`, and `provider_fallback` events from `agent::Loop`.
  - Wired `RuntimeAssembly::hook_bus()` and scope/agent identity metadata through
    `AgentPromptRunner`.
  - Kept `oran-provider` hook-free; `oran-agent` owns the provider await boundary.
  - Bumped the binary slice tag to `2.0.0-slice126`.

### Design Intent

Provider lifecycle hooks were the next small observability slice after the
configured-route prompt/REPL path could reach the real loop. The implementation
publishes from `agent::Loop`, not `oran-provider`, because the loop already owns
turn ids, scope/agent identity, route execution context, streaming sinks, trace
policy, and fallback attribution. That keeps the provider domain reusable and
hook-free while giving hook observers the stable metadata needed for later
usage/cost rollups.

The payloads deliberately carry only metadata: route/profile/model/protocol,
message/tool counts, retry knobs, usage, stop/error classification, and timing.
Prompt bytes, message content, provider bodies, headers, and credentials stay out
of hook delivery.

### Files Modified

- `include/oran/hook/payload.hpp`
- `include/oran/agent/loop.hpp`
- `src/oran-agent/loop.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `xmake/targets.lua`
- `tests/hook/test_bus.cpp`
- `tests/agent/test_loop.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `docs/STATUS.md`
- `docs/QUALITY_SCORE.md`
- `docs/ARCHITECTURE.md`
- `docs/BUILD_SYSTEM.md`
- `docs/design-docs/api-portability.md`
- `docs/design-docs/permissions-and-hooks.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/design-docs/agent-platform.md`
- `docs/releases/feature-release-notes.md`
- `docs/histories/2026-06/20260601-0240-provider-lifecycle-hooks.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 126 snapshot, last-history pointer, next-slice routing,
  and focused validation counts.
- `docs/QUALITY_SCORE.md` — hook, provider, agent, bootstrap, test-count, and
  follow-up text for provider lifecycle hooks.
- `docs/ARCHITECTURE.md` — library inventory and dependency surface for
  `oran-agent -> oran-hook`, plus the hook-free `oran-provider` boundary.
- `docs/BUILD_SYSTEM.md` — xmake dependency example and build-boundary rationale.
- `docs/design-docs/api-portability.md` — provider hook status and v1.1 rewrite
  boundary.
- `docs/design-docs/permissions-and-hooks.md` — provider payload semantics,
  redaction/privacy policy, and per-agent bus wiring.
- `docs/design-docs/bootstrap-runtime.md` — `AgentPromptRunner` bus/identity
  threading.
- `docs/design-docs/agent-platform.md` — cost-awareness path now starts from
  provider lifecycle metadata.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build test-hook`
  - `xmake run test-hook` — 30 cases / 207 assertions
  - `xmake build test-agent`
  - `xmake run test-agent` — 50 cases / 10 689 assertions
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap` — 77 cases / 380 assertions
  - `xmake build orangutan`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - Agent loop request/response provider hook emission.
  - Agent loop provider error hook emission.
  - Agent loop fallback hook attribution when execution serves a fallback profile.
  - Bootstrap `AgentPromptRunner` bus wiring through `RuntimeAssembly`.
  - Hook payload visitor coverage for provider lifecycle alternatives.
- Bench impact:
  - None; no bench target changed.
- Compile-budget delta:
  - Small `oran-agent` dependency/implementation delta; no budget file change.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
  (`provider-lifecycle-hooks`).

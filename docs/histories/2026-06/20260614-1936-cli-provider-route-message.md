## [2026-06-14 19:36] | Task: CLI provider-route message

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: local `orangutan-refactor` workspace
- Linked plan: none; small user-visible correction after slice 245

### User Query

> The no-config CLI output still says the agent loop is not implemented, despite
> configured provider routes already driving `agent::Loop`; also keep
> provider-specific smoke-test labels out of durable protocol docs.

### Changes Overview

- Areas: `oran-cli`, bootstrap/provider docs, status/release docs.
- Key actions: replaced the no-runner CLI placeholder text with an explicit
  "no provider route configured" message, added a CLI stdout regression test,
  clarified that `profiles.<name>.protocol` selects the actual wire protocol,
  and bumped the binary slice tag to `2.0.0-slice246`.

### Design Intent

`agent::Loop` already runs through `AgentPromptRunner` when bootstrap resolves a
configured provider route. The old fallback message was a leftover from the
pre-handoff slices and made a runnable configured-route runtime look incomplete.
The no-route path still stays useful for fresh checkouts, but it now reports the
real boundary: no provider route was configured, so no provider-backed loop was
started. API portability docs now keep vendor labels separate from protocol
selection; Anthropic-compatible endpoints should be configured with
`protocol: "anthropic_messages"` regardless of the provider label.

### Files Modified

- `src/oran-cli/cli.cpp`
- `tests/cli/test_cli.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/cli-runtime.md` - no-route CLI fallback now describes the
  missing provider route instead of implying the agent loop is absent.
- `docs/design-docs/bootstrap-runtime.md` - configured-route prompts are called
  out as already running through `AgentPromptRunner` / `agent::Loop`.
- `docs/design-docs/api-portability.md` - protocol selection is documented as
  explicit `ProtocolKind` configuration; Anthropic-compatible endpoints use
  `anthropic_messages`.
- `docs/STATUS.md` - slice 246 snapshot and validation.
- `docs/releases/feature-release-notes.md` - user-visible release note.

### Validation

- Commands run:
  - `xmake build test-cli && xmake run test-cli`
  - `xmake run orangutan -- --help`
  - `xmake run orangutan -- --prompt "2+2"`
  - provider-specific label scan over current docs, excluding references
  - `git diff --check`
  - `make ci`
- Tests added/changed: CLI stdout regression for no-route single-shot prompts.
- Bench impact: none.
- Compile-budget delta: not measured; no new dependencies or public includes.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md#2026-06`.

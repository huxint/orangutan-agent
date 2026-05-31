## [2026-06-01 03:39] | Task: slice 128 — CLI REPL slash commands

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local shell, xmake release`
- Linked plan: none — single-slice follow-up selected from `docs/STATUS.md`

### User Query

Continue the doc-first architecture/progress pass, keep the execution plan moving, and
implement the next coherent runtime slice only after reading the relevant docs.

### Changes Overview

- Areas: `oran-cli`, CLI/bootstrap docs.
- Key actions:
  - Added REPL slash-command recognition before prompt-runner dispatch.
  - Implemented `/help`, `/exit`, and `/quit` for scripted and interactive REPL
    paths.
  - Kept slash commands from incrementing `prompts_processed` or reaching
    `PromptRunner`.
  - Bumped the binary slice tag to `2.0.0-slice128`.

### Design Intent

Spec 0001's REPL sketch already promised `/help`, and slice 125 made the configured
provider route an actual terminal REPL. This slice adds only the command vocabulary
that has a stable CLI-owned target today: list local commands and exit the loop.

The command parser stays inside `oran-cli` because these commands are terminal
controls, not agent prompts. More slash-command targets should wait until bootstrap or
runtime exposes concrete operations for them; this slice deliberately avoids inventing
configuration, trace, session, or memory commands ahead of their owners.

### Files Modified

- `src/oran-cli/cli.cpp`
- `tests/cli/test_cli.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `README.md`
- `docs/STATUS.md`
- `docs/QUALITY_SCORE.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/cli-runtime.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/design-docs/index.md`
- `docs/product-specs/0001-core-react-loop.md`
- `docs/releases/feature-release-notes.md`
- `docs/histories/2026-06/20260601-0339-cli-repl-slash-commands.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 128 snapshot, last-history pointer, next-slice routing,
  and focused validation count.
- `docs/QUALITY_SCORE.md` — CLI/test counts and remaining line-editor follow-up.
- `docs/ARCHITECTURE.md` — `oran-cli` inventory now includes REPL slash commands.
- `docs/design-docs/cli-runtime.md` — REPL command semantics for scripted and
  interactive inputs.
- `docs/design-docs/bootstrap-runtime.md` — configured-route handoff now documents
  CLI-owned command handling before `AgentPromptRunner`.
- `docs/design-docs/index.md` — CLI-runtime catalogue wording.
- `docs/product-specs/0001-core-react-loop.md` — REPL scope and `/help` behavior.
- `docs/releases/feature-release-notes.md` — user-visible release note.
- `README.md` — quick-start REPL exit/help note.

### Validation

- Commands run:
  - `xmake build test-cli`
  - `xmake run test-cli` — 26 cases / 205 assertions
- Tests added/changed:
  - Scripted no-runner REPL `/help`, `/exit`, and `/quit` handling.
  - Async scripted REPL slash commands before runner dispatch.
  - Interactive REPL slash commands before runner dispatch.
- Bench impact:
  - None; no bench target changed.
- Compile-budget delta:
  - Small `oran-cli` implementation delta; no budget file change.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
  (`cli-repl-slash-commands`).

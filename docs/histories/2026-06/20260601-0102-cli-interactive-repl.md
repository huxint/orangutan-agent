## [2026-06-01 01:31] | Task: slice 125 — CLI interactive REPL handoff

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local shell, xmake release`
- Linked plan: none — single-slice follow-up selected from `docs/STATUS.md`

### User Query

Continue the doc-first architecture/progress pass, keep the execution plan moving, and
implement the next coherent runtime slice only after reading the relevant docs.

### Changes Overview

- Areas: `oran-cli`, `oran-bootstrap`, CLI/bootstrap docs.
- Key actions:
  - Added `CliOptions::interactive_repl` as an explicit gate for terminal stdin
    reads in `cli::run_async`.
  - Enabled that gate only on the configured-route bootstrap handoff that supplies
    `AgentPromptRunner`.
  - Implemented provider-backed REPL line reads through a persistent asio stdin
    descriptor/stream buffer.
  - Preserved scripted `repl_lines` and no-runner shell behavior as deterministic,
    nonblocking paths.
  - Bumped the binary slice tag to `2.0.0-slice125`.

### Design Intent

The previous CLI seam already let tests and future drivers send scripted REPL prompts
through a caller-owned `PromptRunner`, but ordinary configured-route no-prompt runs still
stopped after printing the placeholder shell. This slice closes that user-facing gap
without adding line editing, command parsing, or a new CLI ownership model.

Interactive input is opt-in because `cli::run_async` is also used by tests and
noninteractive callers. Bootstrap sets the flag only when it owns a configured provider
route and an `AgentPromptRunner`; built-in no-route defaults remain a credential-free
deterministic shell. Scripted `repl_lines` stay authoritative even when every scripted
entry is empty, so test harnesses cannot accidentally block on stdin.

### Files Modified

- `include/oran/cli/cli.hpp`
- `src/oran-cli/cli.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/cli/test_cli.cpp`
- `README.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/STATUS.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/design-docs/cli-runtime.md`
- `docs/design-docs/index.md`
- `docs/product-specs/0001-core-react-loop.md`
- `docs/releases/feature-release-notes.md`
- `docs/histories/2026-06/20260601-0102-cli-interactive-repl.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 125 snapshot, last-history pointer, next-slice routing,
  latest library counts.
- `docs/QUALITY_SCORE.md` — CLI/bootstrap status, test count updates, next-step text.
- `docs/ARCHITECTURE.md` — CLI row and bootstrap handoff note for interactive REPL.
- `docs/design-docs/cli-runtime.md` — public API, REPL semantics, bootstrap handoff,
  next steps.
- `docs/design-docs/bootstrap-runtime.md` — configured-route no-prompt REPL behavior
  and remaining follow-ups.
- `docs/design-docs/index.md` — CLI-runtime catalogue wording.
- `docs/product-specs/0001-core-react-loop.md` — REPL scope and AC3 status.
- `docs/releases/feature-release-notes.md` — user-visible release note.
- `README.md` — quick-start command for configured-route REPL.

### Validation

- Commands run:
  - `xmake build test-cli`
  - `xmake run test-cli` — 23 cases / 176 assertions
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap` — 76 cases / 355 assertions
  - `xmake build orangutan`
- Tests added/changed:
  - Interactive REPL reads multiple buffered lines until an empty line.
  - EOF after a partial line dispatches that line once.
  - Scripted REPL input wins over interactive stdin.
  - Empty scripted spans do not fall through to interactive stdin.
  - `interactive_repl=true` without a runner remains nonblocking.
  - Bootstrap configured-route no-prompt REPL dispatch reaches the local provider
    handoff and sends the typed prompt in the HTTP request body.
- Bench impact:
  - None; no bench target changed.
- Compile-budget delta:
  - Small CLI/bootstrap-only implementation delta; no budget file change.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` (`cli-interactive-repl`).

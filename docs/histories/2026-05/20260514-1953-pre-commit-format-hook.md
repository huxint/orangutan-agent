## [2026-05-14 19:53] | Task: pre-commit format hook

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI, /home/huxint/projects/orangutan-refactor`
- Linked plan: none; small workflow/tooling change

### User Query

> Add a pre-commit formatting hook to the prompt-engineering framework and provide a
> clang-format configuration.

### Changes Overview

- Areas: repository workflow, C/C++ formatting, local Git hooks.
- Key actions: added root `.clang-format`; added versioned `.githooks/pre-commit`; documented
  the hook behavior in the workflow/style docs and README quick start.

### Design Intent

The hook formats staged C/C++ files before commit and re-stages the formatted content, but
it refuses to run when those same files also have unstaged hunks. That keeps formatting
automatic without accidentally widening the commit. Shell files remain check-only via
`shfmt -d`, matching the existing workflow rule.

### Files Modified

- `.clang-format`
- `.githooks/pre-commit`
- `README.md`
- `docs/rules/code-style.md`
- `docs/rules/workflow.md`
- `docs/histories/2026-05/20260514-1953-pre-commit-format-hook.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `README.md` — records `.clang-format`, `.githooks/`, and hook installation in the quick start.
- `docs/rules/code-style.md` — points formatting enforcement at the versioned hook.
- `docs/rules/workflow.md` — documents the exact pre-commit hook behavior.

### Validation

- Commands run: `clang-format --style=file --dump-config`; `bash -n .githooks/pre-commit`;
  `git diff --check`; `make ci`; `.githooks/pre-commit`; `git config core.hooksPath .githooks`.
- Tests added/changed: none; tooling-only change.
- Bench impact: none.
- Compile-budget delta: none.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: none.

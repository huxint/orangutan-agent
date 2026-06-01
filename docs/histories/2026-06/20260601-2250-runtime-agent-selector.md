## [2026-06-01 22:50] | Task: Runtime Agent Selector

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none

### User Query

Continue the prompt-runtime arc after per-agent skill enablement and keep the
repository docs in sync with the shipped runtime selector behavior.

### Changes Overview

- Areas: bootstrap CLI parsing, configured-route prompt startup, permissions,
  skills, tests, and user-facing docs.
- Key actions: bump the binary slice tag to `2.0.0-slice140`; make the existing
  `--mode` / `--agent` selectors feed configured-route `AgentPromptRunner`
  options; reject selector flags on the no-provider deterministic shell unless
  `--explain-rules` is active; and extend bootstrap localhost coverage for
  no-route rejection, per-agent skill filtering, and strict-mode tool denial.

### Design Intent

The selector belongs at the bootstrap runner boundary because that is where the
loaded config, permission materialization, workspace skill snapshot,
session/audit agent key, and provider hook metadata converge. Reusing the
existing `--explain-rules` parser keeps one spelling for operator diagnostics
and ordinary configured-route prompt runs, while the no-route shell stays
deterministic and refuses selector flags that cannot affect a provider-backed
agent loop.

### Files Modified

- `src/oran-bootstrap/bootstrap.cpp`
- `include/oran/bootstrap/bootstrap.hpp`
- `tests/bootstrap/test_bootstrap.cpp`
- `README.md`
- `docs/STATUS.md`
- `docs/design-docs/agent-platform.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/design-docs/permissions-and-hooks.md`
- `docs/product-specs/0008-permissions.md`
- `docs/product-specs/0009-skills.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `README.md` - quick-start example for `--agent` on configured provider runs.
- `docs/STATUS.md` - slice 140 snapshot, last-history pointer, and validation.
- `docs/design-docs/agent-platform.md` - runtime `--agent` mapping into skill
  allowlist selection.
- `docs/design-docs/bootstrap-runtime.md` - configured-route selector semantics
  and no-route rejection.
- `docs/design-docs/permissions-and-hooks.md` - selector reuse outside
  `--explain-rules`.
- `docs/product-specs/0008-permissions.md` - runtime permission baseline and
  agent overlay selection.
- `docs/product-specs/0009-skills.md` - configured-route binary selection of
  per-agent skill allowlists.
- `docs/QUALITY_SCORE.md` - bootstrap/test/skills current-state rows.
- `docs/releases/feature-release-notes.md` - user-visible slice 140 row.

### Validation

- Commands run:
  - `xmake run -y test-bootstrap`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed: `test-bootstrap` now covers no-provider selector
  rejection, configured-route `--agent` skill allowlist filtering, and
  configured-route `--mode strict` denial through a provider tool-use turn.
- Bench impact: no new bench; this is one-shot bootstrap option plumbing and
  test-helper expansion, not a runtime hot-path design choice.
- Compile-budget delta: no public include or target/budget change.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`.

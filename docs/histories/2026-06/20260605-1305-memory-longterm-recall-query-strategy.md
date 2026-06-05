## [2026-06-05 13:05] | Task: Long-term recall query strategy

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none; small follow-up from the deep-review memory tracker.

### User Query

> Deeply understand the architecture/current progress, start the next small
> implementation slice, keep iterating the plan, and commit when finished.

### Changes Overview

- Areas: `oran-config`, `oran-bootstrap`, long-term memory recall policy, prompt
  section-5 memory framing.
- Key actions: added typed `memory.longterm.recall.query_strategy` parsing with
  `prompt_text` as the default and `last_user_message` as the opt-in strategy,
  mapped that policy into `AgentPromptRunnerOptions::longterm_recall`, derived
  recall search text from the latest previous user text when selected, and
  bumped the binary slice tag to `2.0.0-slice167`.

### Design Intent

Slice 166 made configured recall narrower by kind; this slice makes the single
prompt-boundary query more useful for follow-up prompts without changing the
once-before-loop invariant. A combined "current prompt plus recent messages"
strategy was rejected because the current FTS5 `MATCH` path would over-constrain
the lexical query. The shipped `last_user_message` selector is explicit,
deterministic, reads only user text blocks from the existing conversation tail,
and falls back to the current prompt when no previous user message exists.

### Files Modified

- `include/oran/config/config.hpp`
- `include/oran/bootstrap/prompt_runner.hpp`
- `src/oran-config/config.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `tests/config/test_config.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `config.example.json`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/memory-system.md` — documents the shipped query strategy
  while preserving prompt-boundary recall ownership.
- `docs/design-docs/bootstrap-runtime.md` — documents config-to-runner recall
  strategy mapping.
- `docs/design-docs/secrets-and-state.md` — updates the typed config-field list.
- `docs/product-specs/0005-memory-system.md` — updates recall policy scope and
  focused validation counts.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — records section-5
  recall query derivation.
- `docs/rules/prompt-design.md` — preserves the "one search before loop" prompt
  invariant with the new strategy selector.
- `docs/ARCHITECTURE.md` — refreshes config/memory/bootstrap inventory rows.
- `docs/QUALITY_SCORE.md` — refreshes config/bootstrap counts and remaining work.
- `docs/exec-plans/tech-debt-tracker.md` — removes richer query derivation from
  the open memory P3 item.
- `docs/releases/feature-release-notes.md` — adds the operator-visible config
  field release note.
- `docs/STATUS.md` — bumps the current slice/history pointer and focused counts.

### Validation

- Commands run:
  - `xmake build test-config`
  - `xmake run test-config` — 44 cases / 369 assertions
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap` — 111 cases / 824 assertions
  - `git diff --check`
  - `make ci`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help` — reports `orangutan v2.0.0-slice167`
  - `xmake test` — 16/16 test buckets passed
- Tests added/changed: config parser coverage for valid/default/malformed
  `memory.longterm.recall.query_strategy`; enum spelling coverage; bootstrap
  runner coverage proving a follow-up prompt can recall from the previous user
  message.
- Bench impact: none; this is config/query plumbing, not a search
  implementation change.
- Compile-budget delta: not measured; focused rebuilds and the full test build
  stayed within normal local iteration bounds.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: existing memory P3 tracker row remains for gated
  sqlite-vec/vector composition, hybrid ranking, and memory tools.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`.

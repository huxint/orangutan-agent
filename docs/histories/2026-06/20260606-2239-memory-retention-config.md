## [2026-06-06 22:39] | Task: Long-term memory retention config

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI, this checkout
- Linked plan: none

### User Query

Continue implementing the most valuable small slice in the memory runtime workstream,
following the repository's docs-first flow, avoiding benchmark-only churn, and
finishing with a conventional commit.

### Changes Overview

- Areas: `oran-config`, long-term memory docs/status/release notes, bootstrap versioning.
- Key actions:
  - Added `config::LongtermMemoryRetentionConfig` under
    `memory.longterm.retention`.
  - Parsed `forget_after_unused_days`, `importance_floor`,
    `max_records_per_scope`, and `decay_check_interval_hours` with conservative
    defaults and config-time validation.
  - Preserved the nested memory config strict/loose unknown-field behavior for
    retention fields.
  - Documented the operator-facing policy contract while keeping execution,
    automation scheduling, and decay hooks explicitly downstream.

### Design Intent

Slice 182 gave long-term memory a backend decay operation but no operator-facing
policy source. This slice lands the smallest next contract: config can describe
the retention policy with explicit units and validation, while runtime ownership
stays separate. Avoiding an opportunistic startup job keeps policy parsing,
decay execution, and lifecycle hook publishing independently testable.

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `tests/config/test_config.cpp`
- `config.example.json`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 183 snapshot, focused validation, and next-slice candidates.
- `docs/design-docs/memory-system.md` — parsed retention policy and remaining ownership gap.
- `docs/product-specs/0005-memory-system.md` — v1 scope and acceptance status for retention config.
- `docs/design-docs/secrets-and-state.md` — config surface inventory for the retention block.
- `docs/ARCHITECTURE.md` — `oran-config`/memory inventory and remaining decay ownership gap.
- `docs/QUALITY_SCORE.md` — `test-config` counts and coverage status.
- `docs/releases/feature-release-notes.md` — user-visible release note for retention config.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-config`
  - `xmake run test-config` — 48 cases / 429 assertions
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap` — 116 cases / 969 assertions
  - `xmake build orangutan`
  - `xmake run orangutan -- --help` — reports `2.0.0-slice183`
  - `make ci`
- Tests added/changed:
  - `Config::parse extracts memory retention policy`
  - `Config::parse rejects malformed memory retention policy`
  - strict/loose-mode unknown-field coverage for `memory.longterm.retention`
- Bench impact: none; this is config validation, not a hot path.
- Compile-budget delta: no new dependencies or heavy public includes.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`

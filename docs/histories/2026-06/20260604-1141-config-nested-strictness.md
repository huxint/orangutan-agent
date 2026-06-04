## [2026-06-04 11:41] | Task: config nested strictness

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local shell in repository checkout`
- Linked plan: none; this is a small tech-debt follow-up from
  `docs/exec-plans/tech-debt-tracker.md`.

### User Query

> Deeply understand the project architecture and current progress before
> starting the next slice.

### Changes Overview

- Areas: `oran-config`, docs/status/release metadata.
- Key actions: unknown fields inside typed provider profiles, provider pricing,
  routes, and hooks now follow the existing loose-warning / strict-error config
  policy. Reserved but untyped `hooks.sinks` and `hooks.bindings` remain
  accepted placeholders until external hook sink models land. The binary banner
  moved to slice 151.

### Design Intent

The second 2026-05-21 deep-review follow-up kept a small config strictness gap
open until nested provider/agent/hook sections existed. Agent fields were
already covered; this slice threads the same policy through provider
profile/pricing, route, and hook parsing without changing config's public value
types. Hook sink/binding fields stay forward-compatible because the design docs
already reserve them as recognized-but-untyped future models.

### Files Modified

- `src/oran-config/config.cpp`
- `include/oran/config/config.hpp`
- `tests/config/test_config.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/secrets-and-state.md` — documented nested strictness and
  the reserved hook placeholders.
- `docs/exec-plans/tech-debt-tracker.md` — removed the closed config strictness
  bullet from the 2026-05-21 follow-up row.
- `docs/QUALITY_SCORE.md` — refreshed config test counts and current behavior.
- `docs/STATUS.md` — moved the snapshot to slice 151 and synced open debt.
- `docs/releases/feature-release-notes.md` — added the operator-facing config
  strictness note.

### Validation

- Commands run:
  - `clang-format -i src/oran-config/config.cpp include/oran/config/config.hpp tests/config/test_config.cpp`
  - `xmake build test-config`
  - `xmake run test-config`
- Tests added/changed: config coverage for loose warnings and strict failures
  on unknown profile, pricing, route, and hook fields, plus acceptance coverage
  for reserved hook `sinks` / `bindings`.
- Bench impact: none; config load correctness only.
- Compile-budget delta: not measured; touched config/bootstrap TUs remain inside
  the focused build/test path.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`.

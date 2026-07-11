## 2026-07-11 10:32 | Task: Deep-review hardening

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI / multi-agent review`
- Linked plan: none — one contained hardening slice; the broader findings were
  absorbed into `docs/exec-plans/tech-debt-tracker.md` instead of opening a
  same-session execution plan.

### User Query

> Deeply review the project and fix the findings.

### Changes Overview

- Areas: agent scheduler lifetime, HTTP/WebSocket control frames, automation
  webhook security, tool/hook redaction, repository enforcement docs.
- Key actions:
  - kept detached cancellation laggards' scheduler-owned path-lock state alive;
  - made newer-libcurl ping/pong handling explicit and deadline-bounded;
  - rejected unauthenticated non-loopback webhook listener binds;
  - redacted raw `MemoryRemember` input for non-trusted generic tool hooks;
  - corrected docs that described inactive CI/docs-sync/static-analysis gates
    as already enforced, and absorbed the remaining review findings into the
    canonical tech-debt row.

### Design Intent

The review found several production defects plus larger authority/concurrency
gaps. This slice fixes the four high-severity issues with contained ownership or
validation changes and regression tests. Filesystem dirfd confinement, channel
worker retirement/caps, provider state-machine corrections, and hosted-CI
activation remain separate because partial implementations would create false
safety. No committed review artifact was created; the live backlog is the
`review/deep-2026-07-11` tracker row per `rules/deep-review.md`.

### Files Modified

- `src/oran-agent/scheduler.cpp`, `src/oran-agent/_impl/path_lock_table.hpp` —
  shared scheduler-owned path-lock lifetime for detached calls.
- `src/oran-http/websocket.cpp` — explicit deadline-bounded pong writes.
- `src/oran-bootstrap/serve.cpp`, `include/oran/bootstrap/serve.hpp` —
  loopback-only unauthenticated webhook boundary.
- `src/oran-tool/input_redaction.cpp` — `MemoryRemember` hook input redaction.
- `tests/{agent,http,bootstrap,tool}/...` — four focused regressions.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

List every doc edited in the same PR as part of this change. If the change
invalidated a doc and the matching edit is *missing*, the PR is incomplete.

- Scheduler/tool/hook/channel/bootstrap/automation design docs and specs now
  describe the four fixes.
- `docs/ROADMAP.md`, `docs/STATUS.md`, and `docs/QUALITY_SCORE.md` reflect slice
  273 and current test counts.
- `docs/rules/{docs-in-sync,critical-rules,static-analysis,testing-and-bench,compile-budget}.md`
  plus `docs/CICD.md` distinguish active gates from intended hosted gates.
- `docs/exec-plans/tech-debt-tracker.md` owns the unresolved ranked findings.
- `docs/releases/feature-release-notes.md` records user-visible hardening.

### Validation

- Commands run: focused tests during implementation; full `test-http`,
  `test-tool`, `test-agent`, `test-bootstrap`; full `xmake test`; binary help;
  `make ci`; `git diff --check`.
- Tests added/changed: `test-http` 28 / 187, `test-tool` 220 / 2405,
  `test-agent` 58 / 10 796, `test-bootstrap` 188 / 1840. The QQ-gated suite was
  not rerun; its deterministic expected count is 190 / 1880.
- Bench impact: none; fixes are control/lifetime/validation paths.
- Compile-budget delta: no public heavy include or new dependency; release
  incremental builds remained within the existing local workflow.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: `review/deep-2026-07-11`.
- Linked release note: `deep-review-hardening` in
  `docs/releases/feature-release-notes.md`.

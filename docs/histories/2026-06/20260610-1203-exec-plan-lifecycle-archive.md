## [2026-06-10 12:03] | Task: exec-plan lifecycle archive sweep

### Execution Context

- Agent: `Claude Code`
- Base model: `Fable 5`
- Runtime: `Claude Code CLI`
- Linked plan: none — doc-only plan-lifecycle maintenance per
  `docs/PLANS_GUIDE.md` ("Move to `completed/` when shipping"; "doc-only
  sweeps are recorded in a history entry, not a plan").

### User Query

> Archive the historical documents (the stale exec plans), since previous
> agents completed the work but never moved the plans out of `active/`.

### Changes Overview

- Areas: `docs/exec-plans/` lifecycle, `docs/ROADMAP.md` dependency
  frontier, `docs/STATUS.md` active-plan list.
- Key actions: closed and archived two completed-but-still-active exec
  plans, and reconciled the ROADMAP rows / Dependency Frontier entries that
  referenced them (plus the frontier rows the slice-228 routing landing had
  already resolved).

### Design Intent

Both plans had reached their stated goals without being closed, which made
`STATUS.md`'s active-plan list misrepresent the project's working set:

- `2026-06-07-automation-cron-category.md` — its goal ("cron-category
  planning and state boundaries without starting a scheduler service")
  shipped across slices 197–225. The remaining service-loop/daemon items
  are explicitly out of its scope and gated on Dependency Frontier #2, so
  the plan closes complete; the Automation ROADMAP row keeps the frontier.
- `2026-06-06-replace-webui-with-desktop.md` — all four docs-repivot
  milestones verifiably shipped (`DESKTOP.md` exists, `FRONTEND.md` gone,
  `check-docs.sh` updated, reference/comment sweeps return only the
  deliberately-kept mentions). Its progress log was reconciled post-hoc and
  the plan closes complete; the first `oran-desktop` code slice was never
  in its scope.

The sweep also updates ROADMAP rows that slice 228 had implicitly
unblocked: Dependency Frontier #1's generic ingress chain (mock adapter +
bootstrap routing) is resolved with the QQ port continuing in its own plan,
frontier #6 (desktop docs-plan completion) is resolved, and the Agent loop /
Agent teams rows now hang on the runtime-service owner (frontier #2) rather
than the routing seam.

### Files Modified

- `docs/exec-plans/active/2026-06-07-automation-cron-category.md` →
  `docs/exec-plans/completed/2026-06-07-automation-cron-category.md`
  (closing note added)
- `docs/exec-plans/active/2026-06-06-replace-webui-with-desktop.md` →
  `docs/exec-plans/completed/2026-06-06-replace-webui-with-desktop.md`
  (closing note added; progress log reconciled with verification notes)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — active exec-plans list now holds only the QQ-port
  plan; history pointer bumped.
- `docs/ROADMAP.md` — Automation and Desktop rows re-pointed at the
  completed plans with refreshed next steps; Agent loop and Agent teams
  rows updated off frontier #1; Dependency Frontier #1 marked resolved for
  the generic chain and #6 marked resolved.

### Validation

- Commands run:
  - `make ci` (docs, hygiene, docs-sync, STATUS freshness, deps, preamble)
  - Verification greps for the desktop plan's sweep criteria (no
    `web UI` / `oran-web` under `src/`, `include/`, `tests/`;
    `docs/FRONTEND.md` absent; `check-docs.sh` lists `DESKTOP.md`)
- Tests added/changed: none — doc-only; no C++ change (binary stays
  `2.0.0-slice228`).
- Bench impact: none.
- Compile-budget delta: none.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none — the `web` → `desktop` config-block migration
  row already exists in `exec-plans/tech-debt-tracker.md`.
- Linked release note: none (not user-visible).

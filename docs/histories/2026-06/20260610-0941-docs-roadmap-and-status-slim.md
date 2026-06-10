## [2026-06-10 09:41] | Task: docs roadmap and STATUS slim-down

### Execution Context

- Agent: `Claude Code`
- Base model: `Fable 5`
- Runtime: `Claude Code CLI`
- Linked plan: none — docs-only slice, executed directly per user decision.

### User Query

> Reorganize the development-collaboration documentation framework so other
> agents can take over and deliver: slim STATUS.md back to a one-screen
> snapshot, add a per-subsystem progress matrix (current slice → next step →
> pre-dependencies), and sync the CLAUDE.md routing.

### Changes Overview

- Areas: `docs/` framework (STATUS, new ROADMAP, spec index, docs-in-sync
  rule), `CLAUDE.md`, `scripts/check-docs.sh`.
- Key actions: created `docs/ROADMAP.md` (18-track progress matrix +
  Dependency Frontier list); cut `docs/STATUS.md` from 1927 lines to a true
  snapshot by deleting the per-slice narrative middle (slices ~53–196) whose
  content is canonical under `docs/histories/`; refreshed the stale
  all-`drafted` Status column in `docs/product-specs/index.md`; registered
  the new ritual and required file.

### Design Intent

STATUS.md had accreted ~1770 lines of reverse-chronological slice narrative
that duplicated `docs/histories/` and buried the actual snapshot, and no doc
answered "which track is at which slice, what is next, and what blocks it" —
the exact questions a handoff agent asks. The split keeps the two update
cadences separate: STATUS.md changes every slice (snapshot), ROADMAP.md only
when a frontier moves (trajectory). Frontier/dependency facts embedded in the
deleted narratives ("X remains downstream", "gated on Y") were harvested into
ROADMAP's matrix and Dependency Frontier before deletion. Alternatives
rejected: matrix inside STATUS.md (forces churn, regrows the file);
per-design-doc status sections with index aggregation (30+ files, manual
aggregation rots, handoff still requires many opens). No automated ROADMAP
freshness gate yet — the ritual rides docs-in-sync review; add tooling only
if it rots.

### Files Modified

- `docs/ROADMAP.md` (new)
- `docs/STATUS.md`
- `docs/product-specs/index.md`
- `docs/rules/docs-in-sync.md`
- `CLAUDE.md`
- `scripts/check-docs.sh`
- `docs/histories/2026-06/20260610-0941-docs-roadmap-and-status-slim.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ROADMAP.md` — new per-track progress matrix: frontier, next step,
  pre-dependencies, refs; plus the cross-track Dependency Frontier list and
  its same-commit update rule.
- `docs/STATUS.md` — slimmed to the snapshot sections; lists all three
  active exec-plans; "How To Update" now includes the ROADMAP row refresh;
  tech-debt lift re-synced against the tracker (desktop row re-dated
  2026-06-06).
- `docs/product-specs/index.md` — honest Status column
  (`drafted` / `in progress` / `v1 shipped` with slice evidence), status
  value conventions, ROADMAP cross-link.
- `docs/rules/docs-in-sync.md` — new table row: a slice that moves a
  subsystem frontier updates the matching ROADMAP row.
- `CLAUDE.md` — Core Workflow step 1 and the Read-At-Start table now route
  through `docs/ROADMAP.md` (AGENTS.md is a symlink and follows).
- `scripts/check-docs.sh` — `docs/ROADMAP.md` added to the required-file
  list.

### Validation

- Commands run:
  - `make ci`
- Tests added/changed: none — docs and CI-script list change only; no C++
  build or behavior change.
- Bench impact: none.
- Compile-budget delta: none.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none new. If ROADMAP.md rows are observed to rot,
  consider a freshness check (e.g. grep the current slice number) in a later
  slice.
- Linked release note: none — not user-visible runtime behavior.

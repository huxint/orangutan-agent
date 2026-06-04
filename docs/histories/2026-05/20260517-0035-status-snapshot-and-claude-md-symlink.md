## [2026-05-17 00:35] | Task: add STATUS.md project-state snapshot + freshness gate

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — prompt-framework foundation.

### User Query

> 检查提示词框架是否还有不足，确保后续新开的会话能够了解项目进度;
> 顺便修正 `CLAUDE.md` 不要写「去阅读 AGENTS.md」, 而是直接链接两者;
> 并且检查提示词框架是否闭环、是否冗余.

### Changes Overview

- Areas: prompt framework root, `docs/`, `scripts/`.
- Key actions:
  - `CLAUDE.md` is now a symlink to `AGENTS.md`. Loading `CLAUDE.md`
    yields the routing-layer content directly, removing the
    "read X first" indirection the old CLAUDE.md required.
  - New `docs/STATUS.md` — one-screen snapshot of current slice, last
    completed history, active exec-plan (or "none"), per-area
    QUALITY_SCORE summary, latest test/assertion counts per library,
    and the open tech-debt rows. A new session reads this first to
    orient on where the project actually is, without having to crawl
    histories + release notes + exec-plans.
  - New `scripts/check-status-fresh.sh` — fails CI if `STATUS.md`'s
    "Last completed history" pointer is older than the newest file
    under `docs/histories/`. The freshness gate makes a stale
    STATUS.md mechanically loud instead of silently misleading.
  - `scripts/ci.sh` and `scripts/check-docs.sh` were extended to
    require the new file and run the new check.
  - `docs/rules/docs-in-sync.md` table now lists STATUS.md as a
    required update for any history entry that completes a slice or
    moves a `QUALITY_SCORE` row, and the "Mechanical Enforcement"
    section lists `check-status-fresh.sh`.
  - Closed-loop drift fix: `AGENTS.md` and `README.md` said C++23 in
    five places where the rest of the project pins C++26
    (`set_languages("c++26")`, critical rules C17). Updated to C++26.
    Reference snapshots under `docs/references/*` legitimately
    describe the upstream template's C++23 era and stay as-is.
  - `README.md` Quick Start now also points at STATUS.md and labels
    `CLAUDE.md` as a symlink instead of a "one-liner".

### Design Intent

The old framework asked a new session to read AGENTS.md, the
collab-guide, several design docs, the rules, and several histories
to figure out *where the project is right now*. Three onboarding
questions had no canonical home:

1. Which slice are we on?
2. What was the most recent landed change?
3. Is there an exec-plan in flight?

`docs/STATUS.md` answers all three on one screen and is the first
file in AGENTS.md's "Read At The Start Of Every Task" table. The
freshness script keeps it honest — if a slice ships without bumping
STATUS.md, CI rejects the PR with a precise diff of what's stale,
matching the same mechanical-enforcement posture the Prime Directive
already uses elsewhere.

The CLAUDE.md → AGENTS.md symlink eliminates the only stale-pointer
risk in the routing layer itself: previously CLAUDE.md said "Read
AGENTS.md first" and was a separate file that could drift out of
sync. Now there is one file with two filenames.

### Files Modified

- `CLAUDE.md` — replaced 57-byte pointer file with a symlink to
  `AGENTS.md`.
- `AGENTS.md` — fixed C++23 → C++26 in the opening paragraph; added
  `docs/STATUS.md` as the first row of "Read At The Start Of Every
  Task".
- `README.md` — fixed five C++23 → C++26 mentions; relabeled
  `CLAUDE.md` as a symlink in the repo-layout block; added STATUS.md
  to the layout and to Quick Start.
- `docs/STATUS.md` — new file.
- `docs/rules/docs-in-sync.md` — table row for STATUS.md; listed
  `check-status-fresh.sh` under Mechanical Enforcement.
- `scripts/check-status-fresh.sh` — new script (executable).
- `scripts/check-docs.sh` — added `docs/STATUS.md` to the required
  files list.
- `scripts/ci.sh` — added the freshness step.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — new snapshot.
- `docs/rules/docs-in-sync.md` — STATUS.md row + enforcement entry.
- `AGENTS.md` — routing-layer pointer + standard fix.
- `README.md` — Quick Start + layout + standard fix.
- This history entry —
  `docs/histories/2026-05/20260517-0035-status-snapshot-and-claude-md-symlink.md`.

### Validation

- Commands run: `make ci` (passes — including the new
  `check-status-fresh.sh`); grep for `C++23` in current docs
  (remaining mentions are in `docs/references/*` and history
  snapshots, both of which legitimately describe past state).
- Tests added/changed: none — documentation + tooling.
- Bench impact: none.
- Compile-budget delta: none.

### Follow-ups

- Issues to file: none.
- Tech-debt entry: none.
- Linked release note: none (pre-release; framework-only change).
- A follow-up commit will sweep the Prime Directive / Conventions
  redundancy across AGENTS.md, REPO_COLLAB_GUIDE.md,
  critical-rules.md C16, and code-style.md so each rule has exactly
  one canonical home and everything else links to it.

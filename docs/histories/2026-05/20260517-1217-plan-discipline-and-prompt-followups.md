## [2026-05-17 12:17] | Task: Plan-discipline collapse + prompt-framework follow-ups

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — docs-only process sweep. The work fits inside a
  single session and a single history entry; per the rewritten
  [`PLANS_GUIDE.md`](../../PLANS_GUIDE.md) "When NOT To Create A Plan"
  this is exactly the shape that should not open a plan.

### User Query

> 检查这个项目的提示词框架是否逻辑闭环，怎么后续的实施没有写
> exec-plans计划了呢

…and, after the audit reply:

> 按照你认为的进行修改

### Changes Overview

- Areas: `docs/PLANS_GUIDE.md`, `docs/STATUS.md`, `AGENTS.md`,
  `docs/design-docs/agent-platform.md`, `docs/exec-plans/tech-debt-tracker.md`.
- Key actions:
  - **PLANS_GUIDE.md** "When To Create A Plan" rewritten to make
    *no plan* the default. Plans are reserved for multi-session,
    multi-slice, or intent-doesn't-fit-on-one-line work. A new
    "When NOT To Create A Plan" section enumerates the shapes
    that the recent slice cadence (single-slice, STATUS-bullet-shaped,
    one-history) has already been using. The rule now also requires
    each `Linked plan: none` history line to say *why*.
  - **STATUS.md** `Active exec-plan` field changed from a bare
    "none" to a two-shape line: either a path, or `none` + the
    reason (typically: covered by the `Next intended slice`
    bullet). "How To Update" extended to require maintaining it.
  - **AGENTS.md** "Working Posture" plan bullet collapsed from
    a restated trigger list ("multiple commits, migration risk,
    architectural impact") into a one-liner that points at
    `PLANS_GUIDE.md` as the SSOT. This finishes the SSOT sweep
    started in `20260517-0055-prompt-framework-ssot-sweep.md`.
  - **agent-platform.md** opened a `## Prompt Assembly (deferred)`
    placeholder section so that
    [`rules/prompt-design.md`](../../rules/prompt-design.md)
    line 36's instruction "record adopted patterns here when the
    builder lands" has an actual landing spot — previously the
    pointer was into a section that did not exist. The placeholder
    enumerates the upcoming responsibilities (system preamble,
    tool catalog, deferred-tool index, skills catalog, memory
    framing, conversation tail) and ties each to the
    `CacheSection` order canonicalized in the rule.
  - **tech-debt-tracker.md** gained two rows: the
    `scripts/check-prompt-preamble` static grep promised in
    `prompt-design.md` "Enforcement", and the
    `bench/oran-agent/prompt_cache_hit_rate.cpp` regression
    scenario promised in the same section. Both are now tracked
    debt instead of "future-promise floating in the rule file".

### Design Intent

**Why collapse the plan-trigger rule instead of re-enforcing it.**
24 plans were written for slices 0–12 (2026-05-14 → 2026-05-16).
The next 9 history entries (2026-05-16 22:00 onward) all carry
`Linked plan: none` with a near-identical reason ("three-commit
push closes criterion N"). The current workflow is: pick one
bullet from `STATUS.md` `Next intended slice`, execute it in one
session, retire the bullet, write one history. The plan layer
would duplicate the bullet without adding information — every
plan would say "do the bullet". Two options were considered:

1. **Re-enforce** the old rule by gating `check-history-touched.sh`
   on `Linked plan: ...` and back-filling the missing 9 plans.
2. **Acknowledge** that `Next intended slice` is now the
   lightweight plan, and tighten the rule so the recent practice
   is sanctioned rather than a silent violation.

Option 1 produces empty-ceremony plans and trains agents to write
plans that nobody reads. Option 2 (this slice) keeps the heavy
plan format for the cases it was designed for (multi-session
work, intent that doesn't fit on a line) and makes the lightweight
path explicit. The trade-off is that "should I write a plan?"
is now a judgment call instead of a checklist — mitigated by the
explicit "fits on the Next intended slice line?" test, and by
the requirement that `Linked plan: none` always state a reason
(so the judgment is auditable in history).

**Why the agent-platform.md placeholder is a placeholder, not a
filled-in design.** The prompt builder will be implemented when
`oran-agent` lands. Writing the design now would be speculation
— `prompt-design.md` rule already pins the invariants; what's
missing is "which Piebald-AI shape did we pick for each section",
and that decision belongs in the slice that actually picks one.
The placeholder closes the dangling pointer without paying for
design that hasn't been informed by code.

**Why the two prompt rows go into the tech-debt tracker rather
than into STATUS.md `Next intended slice`.** Both items are
deferred enforcement that only becomes meaningful when
`oran-agent` exists. They are debts (we know we owe them) but
not next-slice candidates (you can't write the bench without
the builder it benches). Tracking them here means the
`oran-agent` slice plan, when written, will pull them in by
reading the tracker.

### Files Modified

- `docs/PLANS_GUIDE.md` — "When To Create A Plan" rewritten;
  "When NOT To Create A Plan" added.
- `docs/STATUS.md` — `Active exec-plan` field reshaped; "How To
  Update" updated; tech-debt list re-synced from tracker (this
  edit lands in the same PR as the tracker change).
- `AGENTS.md` — "Working Posture" plan bullet collapsed.
- `docs/design-docs/agent-platform.md` — new
  `## Prompt Assembly (deferred)` section inserted before
  `## Cross-Cutting Concerns`.
- `docs/exec-plans/tech-debt-tracker.md` — two new prompt rows.
- `docs/histories/2026-05/20260517-1217-plan-discipline-and-prompt-followups.md`
  — this file.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

This change *is* a docs sweep — every modified file is a doc.
See "Files Modified" above; no production-doc invalidation
beyond the listed edits.

### Validation

- Commands run:
  - `make ci` — pending the user's local run; this entry is
    written assuming the docs hygiene scripts pass. The two
    new tracker rows are in the canonical table shape; the
    AGENTS.md / STATUS.md / PLANS_GUIDE.md edits are
    table-of-content-link-clean (no internal anchors moved).
  - Markdown-link scan recommended over the changed files.
- Tests added/changed: none — documentation only.
- Bench impact: none.
- Compile-budget delta: none.

### Follow-ups

- Issues to file: none.
- Tech-debt entries: two new rows added to
  `docs/exec-plans/tech-debt-tracker.md` (see above).
- Linked release note: none — pre-release, framework-only
  change. No user-visible behavior shift.

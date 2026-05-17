# Execution Plans Guide

Use **execution plans** for tasks that are too large, risky, or stateful to manage
through short chat context.

## When To Create A Plan

The default is **no plan**. A single-session slice whose intent fits inside the
[`STATUS.md`](STATUS.md) `Next intended slice` bullet *and* lands as one history
entry does not need an exec-plan — the bullet + the history together carry the
same information a one-shot plan would, with less ceremony.

Create a plan when **any** of these apply:

- The work spans **multiple sessions** (i.e., another agent must pick it up from
  a cold start without the current chat).
- The work spans **multiple slices** — i.e., more than one history file is
  expected before the goal is reached. A 3-commit slice that lands one history
  is not "multiple slices"; a refactor that needs 3 separate slices is.
- The intent does not fit on the `Next intended slice` line — alternatives have
  to be compared, milestones staged, or measurement checkpoints scheduled
  (compile-time improvements, perf changes, staged rollouts).
- The work migrates from a legacy shape and there is real risk of half-migration
  (e.g., porting QQ adapter to v2 trait).
- The blast radius is wide enough that reviewers / future agents need a
  named place to leave decisions (typically: new library, new public API
  with > 1 caller landing later, dependency change that ripples through
  multiple libraries).

A rough proxy: if the slice closes in *this* session with *one* history file,
and the `Next intended slice` bullet correctly described it before you started,
the plan would just duplicate the bullet — skip it.

## When NOT To Create A Plan

- Single-slice work pre-described by `STATUS.md` `Next intended slice`.
- Targeted bug fix, even multi-commit, when the fix is mechanical.
- Doc-only sweeps (those are recorded in a history entry, not a plan).
- Anything where you would write the plan and the history in the same minute —
  the history is the canonical record; the plan would be empty ceremony.

If you skipped a plan, the matching history's `Linked plan: none` line must say
**why** — typically a one-liner pointing at the `Next intended slice` bullet
it executed against.

## Storage

- Active plans: `docs/exec-plans/active/`.
- Completed plans: `docs/exec-plans/completed/`.
- Template: `docs/exec-plans/templates/execution-plan.md`.
- Ongoing tech debt: `docs/exec-plans/tech-debt-tracker.md`.

Create a new plan with:

```sh
make new-plan SLUG=mvp-react-loop
```

This drops `docs/exec-plans/active/<YYYY-MM-DD>-mvp-react-loop.md` from the template.

## Expected Sections

The template encodes:

- **Goal** — desired end state.
- **Scope** — in/out of scope.
- **Context** — relevant docs and code paths.
- **Risks** + mitigations.
- **Milestones**.
- **Validation** — commands, manual checks, observability checks.
- **Progress Log** — checklist with timestamps.
- **Decision Log** — dated decisions + rationale.

## Maintenance

- Update the plan as decisions change. Reviewers and the next agent will read the
  *current* state, not the original.
- Move to `completed/` when shipping. Do not delete; the history is valuable.
- If the plan goes stale (no progress in 30 days, no longer relevant) archive it to
  `completed/` with a closing note.

## Anti-Patterns

- Plans that describe *implementation* in detail before any implementation. State
  *intent*, not source code. Source code goes in the PR.
- Plans that bypass design docs. If you find yourself defining a public API in a
  plan, the design doc should host that decision; the plan references it.
- "Living" plans that never close. Close them; open a follow-up if needed.

## See Also

- [`exec-plans/README.md`](exec-plans/README.md)
- [`HISTORY_GUIDE.md`](HISTORY_GUIDE.md)

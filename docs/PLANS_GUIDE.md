# Execution Plans Guide

Use **execution plans** for tasks that are too large, risky, or stateful to manage
through short chat context.

## When To Create A Plan

The default is **no plan**. A contained, single-session change does not need an
exec-plan; the code, tests, and commit message carry enough context.

Create a plan when **any** of these apply:

- The work spans **multiple sessions** (i.e., another agent must pick it up from
  a cold start without the current chat).
- The work spans multiple independently shippable milestones.
- The intent does not fit on the `Next intended slice` line — alternatives have
  to be compared, milestones staged, or measurement checkpoints scheduled
  (compile-time improvements, perf changes, staged rollouts).
- The work migrates from a legacy shape and there is real risk of half-migration
  (e.g., porting QQ adapter to v2 trait).
- The blast radius is wide enough that reviewers / future agents need a
  named place to leave decisions (typically: new library, new public API
  with > 1 caller landing later, dependency change that ripples through
  multiple libraries).

A rough proxy: if the work closes in this session and has one obvious validation
path, a plan would duplicate the commit message — skip it.

## When NOT To Create A Plan

- Single-session work with a narrow owner and obvious validation path.
- Targeted bug fix, even multi-commit, when the fix is mechanical.
- Doc-only sweeps.
- Anything where the plan would merely restate the requested change.

## Storage

- Active plans: `docs/exec-plans/active/`.
- Completed plans are deleted after durable decisions and remaining work have been
  absorbed into current contracts or the tech-debt tracker; Git keeps the archive.
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
- Delete a completed or abandoned plan after absorbing remaining decisions and debt.

## Anti-Patterns

- Plans that describe *implementation* in detail before any implementation. State
  *intent*, not source code. Source code goes in the PR.
- Plans that bypass design docs. If you find yourself defining a public API in a
  plan, the design doc should host that decision; the plan references it.
- "Living" plans that never close. Close them; open a follow-up if needed.

## See Also

- [`exec-plans/README.md`](exec-plans/README.md)

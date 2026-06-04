# Deep-Review Artifacts

> A **deep review** is a comprehensive audit pass — an agent (or sub-agent)
> reads a slice's worth of code + docs against the project's rules and
> emits a single artifact listing every finding. The artifact's value is
> short-lived: it exists to *seed actionable rows in live docs* and is
> then deleted. This rule covers naming, the version stamp, the
> absorption pipeline, and the delete-on-close discipline.

## Why This Rule Exists

Two failure modes have already shown up in this repo:

1. **Stale review files left in the tree.** `orangutan-deep-review.md`
   sat at the repo root after its rank-0 findings were closed by
   slices 31-33. A future agent reading the file would have re-litigated
   findings that no longer matched the code, because the artifact lacked
   a version stamp and never got deleted on absorption.
2. **`/tmp/...` paths cited as live sources.** `docs/STATUS.md` and
   `docs/exec-plans/tech-debt-tracker.md` referenced
   `<tmp-deep-review-artifact>` and
   `<tmp-agent-loop-foundation-artifact>` as the home of
   actionable backlog. Those files only ever existed on one machine — a
   future agent on a fresh checkout cannot read them.

Both failures break the project's *agent-first* posture: every
load-bearing decision must live in a versioned, in-repo file. Review
artifacts are *not* load-bearing; the absorbed rows are. The artifact is
scaffolding and must be torn down.

## Lifecycle

```text
   draft ──▶ in-repo review ──▶ absorption ──▶ deletion
 (/tmp/...)   (docs/reviews/)   (specs +        (file removed
                                 tracker +       same slice that
                                 design-docs)    closes the last
                                                 actionable row)
```

A review may *start* in `/tmp/` for an exploratory pass — that is fine,
nothing in `/tmp/` should ever be cited as a live source from a checked-in
doc. When the review is going to drive multiple follow-up slices, copy
it to `docs/reviews/<name>.md` first, then absorb from there.

If the review never produces a follow-up slice (everything was already
fine), it doesn't need to enter the repo — discard the draft.

## Naming Convention

When a review is committed to the repo, it lives under `docs/reviews/`
with this filename shape:

```text
docs/reviews/YYYY-MM-DD-slice<N>-<short-slug>.md
```

- `YYYY-MM-DD` — calendar date the review was authored.
- `slice<N>` — the slice the review *targets* (the codebase state it
  audited), e.g. `slice33`.
- `<short-slug>` — 1-3 hyphenated words: `tool-runtime`,
  `agent-loop-foundation`, `prompt-cache-discipline`.

The slice number is mandatory because review findings are only valid
against a specific snapshot of the code; an unstamped review becomes
indistinguishable from a current-state design doc within one or two
slices.

## Version Stamp (First Lines)

The first lines of every committed review must declare what it audited.
Minimum template:

```markdown
# Deep Review — <Topic> (Slice <N>, <YYYY-MM-DD>)

> **Target:** slice <N> of `orangutan-agent` (commit `<short-sha>` if known).
> **Author:** <agent name + model + runtime> (e.g. `Claude Opus 4.7 / Claude Code`).
> **Status:** open · absorbing · closed (delete-on-close).
> **Absorption home:** `docs/exec-plans/tech-debt-tracker.md`
> row `review/deep-<YYYY-MM-DD>` + specs `<list>`.
```

A reviewer reading the file two slices later can immediately see the
findings may be stale and where the live versions of those findings now
live.

## Absorption Pipeline

Findings do not stay in the review artifact. They move into the live
docs that will outlast the review:

| Finding shape | Lives in |
| ------------- | -------- |
| Concrete bug / smell with a code target (file:line) | A row in [`exec-plans/tech-debt-tracker.md`](../exec-plans/tech-debt-tracker.md), grouped under `review/deep-<YYYY-MM-DD>` |
| Future-feature recommendation (new tool, new API shape) | A new or extended [`product-specs/*.md`](../product-specs/) |
| Cross-cutting design decision (module boundary, error model) | The matching [`design-docs/*.md`](../design-docs/) |
| New invariant the project must follow | A new or extended [`rules/*.md`](.) |
| User-visible behavior fixed by absorption | A row in [`releases/feature-release-notes.md`](../releases/feature-release-notes.md) |

Every absorbed row should cite the review §number (or anchor) as
**provenance only** — never as a live link, since the source file is
about to be deleted. A row like "deep-review §4.1.1" is fine; a row like
"see [orangutan-deep-review.md](../orangutan-deep-review.md)" is wrong
once the artifact has been deleted.

## Deletion

Once *every* actionable finding from a review has been absorbed (or
explicitly rejected with a recorded reason), the review artifact is
**deleted in the same slice** that closes the last finding. The history
entry for that slice notes the deletion. No archived `.bak` / `.old` /
`.review-N` variants are kept — `git log` is the archive.

This is the load-bearing half of the rule. A review that lingers is
worse than no review, because future agents will re-cite findings that
the code has already addressed.

If a review needs to be kept *because the work is genuinely
multi-quarter*, promote the relevant sections into the matching
`design-docs/` or `product-specs/` and delete the review. The promoted
sections then live under the Prime Directive
([`docs-in-sync.md`](docs-in-sync.md)) and stay current.

## Live-Doc Hygiene

Two practical consequences for `STATUS.md`,
`exec-plans/tech-debt-tracker.md`, and similar current-state docs:

- **Do not cite `/tmp/...` paths.** Those files are ephemeral and
  per-machine. Copy the relevant context into the live doc.
- **Do not link to a review file that may be deleted within the same
  cycle.** Cite by date + slug ("the 2026-05-21 deep review"), and let
  the absorbed rows carry the real content.

History entries (`docs/histories/YYYY-MM/...`) are immutable provenance
and *may* reference deleted artifacts — they record what was true at
the time of the slice, not the current state.

## Enforcement

No mechanical check today — this is review-time discipline. A future
script (working name: *check-no-stale-reviews.sh*) would assert:

- No file matches `orangutan*deep-review*.md` at the repo root.
- Every file under `docs/reviews/` has the version-stamp block in its
  first 10 lines.
- Live docs (`docs/STATUS.md`,
  `docs/exec-plans/tech-debt-tracker.md`,
  `docs/design-docs/*.md`, `docs/product-specs/*.md`,
  `docs/rules/*.md`) contain no `/tmp/...md` substring.

Add the script under `scripts/` and list it under "Mechanical Enforcement"
in [`docs-in-sync.md`](docs-in-sync.md) the first time a fresh deep review
lands in the repo.

## Anti-Patterns

- **Banner-only stale marking.** Adding "⚠️ STALE — see STATUS.md" to
  the top of a review and leaving the file in place. The banner does
  not stop a future agent from extracting the body. Delete the file.
- **Review that names itself the source of truth.** "See section 4.2.1
  of this review for the canonical contract." Reviews are audits, not
  contracts; canonical contracts live in `design-docs/` / `product-specs/`.
- **Multiple review artifacts for the same slice.** A second pass should
  amend the first one and re-stamp the version line, not create a
  parallel `-v2.md` file. The reviewer audits a single snapshot.
- **Reviews that double as plans.** A review surfaces problems; an
  exec-plan organises a multi-slice response. If you find yourself
  scheduling milestones inside a review, the right move is to open a
  plan in `docs/exec-plans/active/` and shrink the review back to
  findings.

## See Also

- [`investigation.md`](investigation.md) — discipline for the
  *research-before-code* phase; deep reviews are the *audit-after-code*
  phase and share the same anti-patterns (fan-out > 2, snippet
  blind-copy, stale-knowledge).
- [`docs-in-sync.md`](docs-in-sync.md) — the Prime Directive. The
  deletion clause above is its corollary for review artifacts.
- [`../HISTORY_GUIDE.md`](../HISTORY_GUIDE.md) — the slice that closes a
  review records the deletion in its history entry.
- [`../PLANS_GUIDE.md`](../PLANS_GUIDE.md) — when a review's follow-up
  is multi-slice, an exec-plan is the right next artifact, not a
  longer review.
- [`../exec-plans/tech-debt-tracker.md`](../exec-plans/tech-debt-tracker.md)
  — the canonical absorption home for review findings.

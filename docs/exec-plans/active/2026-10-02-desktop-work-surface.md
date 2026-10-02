# Desktop work surface and brand

## Objective

Rework the desktop UI as a restrained, fluid work surface. Remove the boxed
dashboard treatment, improve agent execution fidelity and replace the geometric
badge with an organic, monochrome Orangutan mark. Preserve stored user data,
source restrictions, five-page navigation and the simulated execution boundary.

## Research and design decisions

- [Cowork](https://claude.com/product/cowork): goals, visible execution and
  reviewable deliverables; intervention stays available.
- [Jan Cowork](https://www.jan.ai/docs/desktop/cowork): adjacent progress and
  artifact review preserves conversation context. Its documentation labels this
  a nightly preview, not a stable production baseline.
- [Linear agents](https://linear.app/agents): human ownership, agent contribution,
  compact summaries with underlying details available.

Palette: fog #f2f2ef, canvas #fcfcfa, ink #30332f, secondary #70756f,
copper #a35f43 and muted green #507461. Use matching semantic dark tokens.
System sans typography, a moderate 30px heading and 14px body; no display serif,
promotional headline, decorative gradient or avatar illustration.

The desktop layout is an open canvas beside a quiet navigation/history column.
The empty session has a primary input column and a smaller work-activity column;
on narrow screens they stack. Lower-frequency navigation moves below history.
Spacing and subtle surface differences replace pervasive rules and card borders.
Only input, overlays and approvals need stronger containment.

Motion responds to user actions: soft pressed states, short page/panel transitions,
theme and tab feedback. No perpetual idle animation; reduced motion disables
nonessential movement. Drawer navigation keeps focus inside its open surface.

## Complete slice

1. Replace shared brand SVG, shell and visual tokens; restyle all five pages.
2. Compact task summaries and truthful status-based execution detail; replace
   fabricated line-added diff with literal source view.
3. Place task/approval presentation with the initiating turn; preserve local-only
   commands. Improve scrolling without stealing the reader's position.
4. Check desktop/narrow, bilingual/light/dark, keyboard and reduced motion.

No provider, native host, storage schema or runtime changes. Existing fixture and
user content remains preserved; demonstration limits stay visible but unobtrusive.

## Verification

Frontend build/format, model tests, browser tests and make ci. Add focused browser
coverage for changed interaction behavior and inspect actual rendered pages.
Update product/implementation owners, delete the completed plan and commit.

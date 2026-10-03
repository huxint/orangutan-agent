# Desktop surface redesign

## Objective

Rebuild the desktop prototype's presentation layer as a systematic surface
language and finish the conversation reading experience. The visual direction
stays what the product specification owns: silver-white canvas, light-silver
navigation, restrained deep-blue accent, text wordmark, graphite dark theme.
Preserve all commands, user snapshots, source restrictions and the simulated
execution boundary.

## Audit findings (live, all five pages, both themes, both languages, narrow)

- Light-mode secondary text misses WCAG AA on all three surfaces
  (muted token 4.04–4.37 contrast); dark mode passes.
- Settings content starts 64 px right of every other page (double max-width);
  the topbar gutter (30 px) matches no page gutter (72 px).
- Search results show raw connection IDs (`qq-main`) instead of display names.
- The conversation date placeholder repeats the scroll-region label instead of
  showing a date; the streaming caret renders detached under the last block.
- Radii (8–22 px), type sizes (10–34 px) and two near-identical selection
  color pairs are duplicated across rules; component controls drift apart.
- Team progress in the task card expresses member state by nothing but roles;
  state must pair text with an icon rather than color alone.

## Design

Foundation tokens (extension of the existing block, dark mirrors):
type scale 11/12/13/14/15/18/26/hero; radius scale 6/8/10/12/16 applied to
inputs, rows, cards, overlays; one `--gutter` shared by topbar and pages;
`--muted` darkened to pass AA on canvas, panel and sidebar; `--nav-active*`
removed in favor of the single selected pair; `time` leaves the monospace
group and uses tabular figures.

Frame: one content measure for all pages, aligned topbar, tighter sidebar
rhythm. Controls: segmented/tabs/theme options share one metric set; dialogs
16 px radius; composer keeps a definite container by default (white surface,
line border) and only deepens its border and shadow on focus.

Conversation: real date separator, inline streaming caret at the end of the
last rendered block, quieter author line, member chips with state icons in the
task card, approval card on the 12 px scale. Brand: the `o.` mark becomes
two-tone (ink letter, accent period) in logo, assistant avatar and favicon.

Data presentation: search rows use connection display names; activity times at
caption size.

## Contracts

[Desktop UI implementation](../design-docs/desktop-ui.md) owns the current
presentation facts and is updated in the same change; the
[product specification](../product-specs/desktop-prototype.md) visual direction
is unchanged. No schema, command, fixture or persistence changes.

## Verification and completion

Run `npm run check`, `npm run format:check`, `npm run build`, model tests,
Playwright browser tests and `make ci`. Re-run the live audit matrix (light,
dark, Chinese, English, narrow) and compare before/after screenshots. Update
the owning document, delete this plan and commit the verified result.

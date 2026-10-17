# Desktop warm workbench redesign

## Objective

Rework the desktop prototype's visual system into a warm, minimal desktop
application in the spirit of Claude Desktop: a calm parchment canvas, quiet
surfaces, editorial serif display type, and a single warm accent. The identity
stays Orangutan's: the accent is drawn from the subject's world (sienna for
fur and forest), the wordmark and hero set in serif where Claude Desktop's
wordmark is sans, and the task rail encodes task ownership — the product's
own differentiator — instead of copying any third-party layout or brand.

Preserve all commands, snapshots, source restrictions, the simulated execution
boundary, test identifiers and the accessible behavior the current UI already
owns. The change is presentation: one token system, shared component surfaces,
and the conversation reading experience.

## Current contracts

- [Desktop UI implementation](../design-docs/desktop-ui.md) owns the current
  presentation facts: silver-white canvas, light-silver navigation, deep-blue
  accent, graphite dark theme, Sarasa-first font stack, token scales.
- [Product specification](../product-specs/desktop-prototype.md) section 7 owns
  the visual direction and its references; it is updated in the same change.
- The browser contrast test pins `--muted`/`--subtext` to WCAG AA on `--bg`,
  `--sidebar`, `--surface`, `--panel` and `--hover` in both themes; tokens must
  stay six-digit hex for that check.
- No schema, command, fixture, persistence or demo-logic changes.

## Design

### Palette (all tokens six-digit hex, dark mirrors warm)

Light: parchment canvas `#f6f2ea`, sand sidebar `#ede7db`, paper surface
`#fbf8f2`, soft panel `#f1ebe0`, wash hover `#e9e2d4`; ink text `#292118`,
subtext `#5b4e3f`, muted `#695a47` (AA on all five surfaces); sienna accent
`#b24a1d` for fills (white on it passes AA), `#8f3e14` as text-safe accent ink;
forest `#3d6b4a`, slate `#476b83`, brick `#a3442e` for semantic states;
warm hairlines `#e7e0d2`/`#dcd3c4`/`#b9ae9c`.

Dark: umber canvas `#211c16`, sidebar `#1a1510`, surface `#2a231b`, panel
`#251f18`, hover `#322a20`; text `#f1eade`, subtext `#d4cbb6`, muted `#a4977f`;
amber-sienna accent `#e08a5d` with dark on-accent; semantic greens/blues/reds
lifted to warm tints. Selected wash becomes sienna-tinted in both themes.

### Typography

Add a local-only serif display stack (`Noto Serif SC`, `Source Han Serif SC`,
`Noto Serif CJK SC`, `Songti SC`, `STSong`, Georgia, serif) applied with
restraint: brand wordmark, home hero, page headings, panel titles and the
about/settings wordmarks. Body stays the Sarasa-first sans stack at 15 px
reading size; no remote fonts. The `o.` mark moves to the serif with an
accent period in logo, avatar and favicon.

### Surfaces and signature

- Replace floating card shadows with hairline borders on warm surfaces;
  buttons stay ink-dark and warm to sienna on hover; composer keeps definite
  containment (paper surface, larger radius, sienna-tinted focus).
- Signature: a task spine rail on the conversation axis — a hairline that
  connects a request to its task card and approval, sienna-tinted while the
  run is active, neutral when settled. It visualizes task ownership, which the
  specification already names as the product's differentiator.
- Selected conversation keeps the sole emphasis background; navigation and
  segmented controls stay on the existing shared metric sets.

## Verification and completion

Run `npm run check`, `npm run format:check`, `npm run build`, model tests,
Playwright browser tests (light, dark, narrow, Chinese, English) and `make ci`.
Compare before/after screenshots in both themes. Update the owning documents
(prototype section 7, desktop-ui sections 1 and 7, theme-color and favicon),
delete this plan and commit the verified result.

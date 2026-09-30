# Desktop Visual Refinement

## Objective

Respond to the user's assessment that the prototype feels immature. Deliver a
compact, restrained assistant workbench with clear typography, useful session
information and source/status visibility. Remove lifestyle illustration, mascot
expression and promotional copy. Preserve demo state and all operation boundaries.

## Scope And Approach

- Replace the welcome promotion with a practical local conversation starting point,
  task activity and compact request suggestions.
- Use neutral light/dark surfaces, a sans-serif hierarchy, limited orange emphasis,
  smaller corner radii and readable metadata. Reduce decorative card repetition.
- Refine the sidebar/wordmark, task inspection, connection and memory layouts.
- Replace UI slogans with direct labels and explanations; preserve saved user text.
- Update the owning product specification and existing browser assertions.
- No new dependencies, runtime integration, permission changes or storage migration.

The [desktop contract](../../product-specs/desktop-prototype.md) remains the
behavioral owner. This coordinated cross-page redesign exceeds the usual small
diff guideline and is recorded before implementation. C++ compile impact is zero.

## Verification

Run frontend type/build/format checks, existing state and browser suites and
`make ci`. Review actual light/dark screenshots at desktop and narrow widths.
Verify local input and channel read-only inspection remain visually distinct.
Record lasting visual requirements in the product specification, remove this
plan when complete and commit the verified slice.

## Progress

- [x] Inspect current composition and identify hierarchy/content problems.
- [ ] Replace visual and content structure across the five pages.
- [ ] Review screenshots and pass affected verification.

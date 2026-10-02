# Desktop typography and working layout

## Objective

Replace the rejected fog-green, oversized rounded workspace with a composed
desktop surface. Preserve all five pages, demo data, command authority and
existing keyboard/lifecycle behavior. User explicitly requests direct design
and implementation without image generation.

## Design

Palette: white #ffffff, silver #f5f6f8, graphite #24272e, secondary #606673,
blue #345bd5, selected #e9edf8. Dark mode uses neutral graphite equivalents.
Use locally available Noto Sans CJK SC and Segoe UI/system sans; 15px reading,
13px controls, 12px metadata. Brand uses a typographic wordmark, no animal avatar.

Single aligned work column: compact heading, composer, textual starting points,
then recent tasks as legible rows. Navigation remains quiet and separate.
Conversation and artifact typography support sustained reading. Limit rounded
containers to inputs and actual overlays; remove the rounded whole-app frame.

The rejected alternative was recoloring the same split home layout. This slice
changes its hierarchy and density instead. No decorative illustrations or
marketing statements. Motion only answers actions; reduced-motion retained.

## Verification and completion

Build, format, model and browser regression, make ci; inspect actual light/dark
and narrow views. Update owning product/design docs, remove this completed plan,
and commit. No runtime integration or schema change.

# Desktop visual redesign

## Objective and scope

Replace the desktop prototype's visual presentation with a coherent workspace
design across its five pages, preserving the existing commands, user snapshots,
source restrictions and simulated execution boundary.

## Design

- Neutral paper canvas #f7f8fa, white content surfaces, ink #242936,
  slate secondary text #657084, restrained orange #b95b32 and warm stone sidebar
  #f0eeea. Dark mode uses matching semantic tokens.
- System sans typography with a readable 14px body and 34px main heading;
  compact secondary navigation, no decorative uppercase labels.
- A left-aligned starting point emphasizes the composer. Three prompt shortcuts
  carry short useful descriptions. Recent activity is a structured list below.
- Navigation, lists, toolbars and task details share spacing and selection rules.
  Color signals actions and state; no promotional hero or ornamental cards.

## Contracts

Desktop product and implementation documents own behavior and presentation.
No native host, provider connection, schema or persistence changes are included.

## Verification and completion

Run frontend build, format check, existing model/browser tests and make ci.
Inspect actual light/dark, English and narrow viewport rendering; keep a running
preview available. Update owning visual documentation and remove this plan once
the verified implementation is committed.

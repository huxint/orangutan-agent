## [2026-05-16 23:20] | Task: document enum-reflection and contains() rules

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — rule-text update.

### User Query

> Update the prompt-framework rules: add rules for #2 (reflection-backed
> enum<->string, no per-enum wrappers) and #3 (`contains` over
> `find != end()` / `find != npos`), so future code changes don't slip
> back into the old syntax.

### Changes Overview

- Areas: `docs/rules/`, `docs/references/`, `AGENTS.md`.
- Key actions:
  - `docs/rules/code-style.md` — rewrote the "Enums" subsection: removed
    the obsolete reference to `magic_enum` (the dependency is gone),
    documented `core::enum_name` / `core::parse_enum` /
    `core::enum_values` as the one canonical helper, codified "no
    per-enum forwarding shims", and called out the trailing-underscore
    convention. Added a new "Membership Tests: contains, not find != end"
    subsection that names every flavor (`std::ranges::contains`,
    associative-container `contains`, `string::contains`) and gives the
    PREFERRED / FORBIDDEN code example.
  - `docs/rules/critical-rules.md` — extended C17's bullet list with
    "Use `contains` for membership tests" and "Use reflection for
    enum<->string mappings", each pointing back to the code-style
    section. Also added `std::ranges::contains` vs. `find != end()` to
    the "newer of two equivalents" examples.
  - `AGENTS.md` — added two bullets to "Conventions At A Glance" mirroring
    the two new rules so the routing layer points new contributors at
    the rule the first time they hit either pattern.
  - `docs/FAST_COMPILATION.md` — removed the `magic_enum` reference;
    replaced with the compile-budget note for the reflection helper.
  - `docs/rules/libraries.md` — removed the `magic_enum` row.
  - `docs/references/third-party-libs.md` and
    `orangutan-legacy-audit.md` — recorded `magic_enum` as legacy-only,
    pointing at the C++26 reflection replacement.

### Design Intent

The rules live where they belong: the *what / why* in `code-style.md`,
the *hard line* in `critical-rules.md`, and the *first-pointer-for-new-
contributors* in `AGENTS.md`. The rules are written so a future PR can
be reviewed against them mechanically — both have a clear FORBIDDEN
example and a single sentence describing when the iterator-form `find`
is still legitimate (callsites that use the iterator after the check).

### Files Modified

- `docs/rules/code-style.md` — rewritten Enums section; new Membership
  Tests section.
- `docs/rules/critical-rules.md` — two new C17 bullets.
- `AGENTS.md` — two new convention bullets.
- `docs/FAST_COMPILATION.md` — replaced `magic_enum` paragraph.
- `docs/rules/libraries.md` — removed `magic_enum` row.
- `docs/references/third-party-libs.md` — marked `magic_enum` as legacy.
- `docs/references/orangutan-legacy-audit.md` — same.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- All rule and convention surfaces touched in the same commit. No
  external doc invalidation outstanding.
- This history entry —
  `docs/histories/2026-05/20260516-2320-rules-enum-reflection-contains.md`.

### Validation

- Commands run: `git diff` (every changed file scanned for dangling
  references to the removed `magic_enum` and the old `to_string_view`
  pattern); `grep -rn 'magic_enum'` confirms only legacy-context
  mentions remain.
- Tests added/changed: none — documentation only.
- Bench impact: none.
- Compile-budget delta: none.

### Follow-ups

- Issues to file: none.
- Tech-debt entry: when `scripts/check-banned-includes.sh` or its
  sibling style linters grow a grep for `find\(.\+\)\s*!=\s*.*\.end()`
  outside iterator-storing patterns, point the script at this rule.
- Linked release note: none (pre-release).

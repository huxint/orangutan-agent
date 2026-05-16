## [2026-05-17 00:55] | Task: SSOT sweep — collapse Prime Directive and convention duplication

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — follow-up to the STATUS.md framework commit.

### User Query

> 同时检查提示词框架是否还有不足 […] 同时减少提示词框架中冗余的信息，
> 做到一遍清晰，而不是各种重复。

### Changes Overview

- Areas: top-level routing layer, repo-collab guide, critical rules.
- Key actions:
  - `AGENTS.md` rewritten so it is purely a routing index. Three
    structural changes:
    1. The Prime Directive callout collapsed from a 12-line restated
       rule into one paragraph pointing at the canonical home in
       `docs-in-sync.md`.
    2. "Conventions At A Glance" became a single-row-per-area table
       where each row is a one-line summary + a link to the rule
       file. No rule is restated; the table is a lookup, not a
       second copy.
    3. "Module Routing" absorbs `REPO_COLLAB_GUIDE.md`'s former
       "Coding Standards (Pointers)" so there is one routing index
       per concept, not two. A new `Working Posture` section keeps
       the four cultural notes that did not fit either table
       (compile-time is a feature, hooks are pluggable, no special
       code, plans first).
  - `REPO_COLLAB_GUIDE.md`: "Documentation Discipline" reduced to one
    paragraph pointing at `docs-in-sync.md` + a short list of
    operating notes that are *not* in the Prime Directive. "Coding
    Standards (Pointers)" deleted (now a one-line pointer at
    AGENTS.md's "Module Routing").
  - `critical-rules.md` C16 collapsed: the rule line stays; the
    rationale paragraph (legacy `orangutan/` had a stale `CLAUDE.md`
    pointing at missing files, etc.) moved into `docs-in-sync.md`
    where it already exists; the enforcement line now names all
    three scripts (`check-docs-sync.sh`, `check-status-fresh.sh`,
    `check-history-touched.sh`).

### Design Intent

Before this sweep the Prime Directive was stated four times — in
AGENTS.md, `REPO_COLLAB_GUIDE.md`, `critical-rules.md` C16, and
`docs-in-sync.md` — with slightly different wording each time. The
convention list ("Console output is `std::print`", "Use ranges",
etc.) was duplicated between AGENTS.md, `code-style.md`, and
`critical-rules.md` C17, again with prose drift. Module routing
lived in both AGENTS.md ("Read When The Task Touches A Module") and
`REPO_COLLAB_GUIDE.md` ("Coding Standards (Pointers)") with
overlapping but not identical contents.

The fix is Single Source of Truth applied mechanically: each rule
lives in exactly one file, and every other surface links to it.
AGENTS.md becomes a pure routing layer — a new session reads
exactly one one-line summary per area and clicks through for the
full text. This matches the prompt-engineering norm that an agent's
top-level prompt should be a navigable index, not an encyclopedia.

### Files Modified

- `AGENTS.md` — rewritten.
- `docs/REPO_COLLAB_GUIDE.md` — "Documentation Discipline" collapsed;
  "Coding Standards (Pointers)" deleted.
- `docs/rules/critical-rules.md` — C16 collapsed to one paragraph + an
  enforcement line that names the three relevant scripts.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `AGENTS.md` — routing-layer rewrite.
- `docs/REPO_COLLAB_GUIDE.md` — collapsed sections.
- `docs/rules/critical-rules.md` — C16 collapsed.
- This history entry —
  `docs/histories/2026-05/20260517-0055-prompt-framework-ssot-sweep.md`.
- `docs/STATUS.md` — `Last completed history` repointed to this file
  so `check-status-fresh.sh` stays green.

### Validation

- Commands run: `make ci` (passes — including `check-status-fresh.sh`);
  a markdown-link scan across the full `*.md` tree (0 broken local
  links).
- Tests added/changed: none — documentation only.
- Bench impact: none.
- Compile-budget delta: none.

### Follow-ups

- Issues to file: none.
- Tech-debt entry: none. `check-docs-sync.sh` still has two planned
  enhancements (code-doc symbol sync, config-shape sync) as before;
  the freshness gate is now real.
- Linked release note: none (pre-release; framework-only change).

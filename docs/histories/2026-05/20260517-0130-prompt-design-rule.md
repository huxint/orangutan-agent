## [2026-05-17 01:30] | Task: add prompt-design rule (Piebald reference + cache discipline)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — prompt-framework forward-looking rule.

### User Query

> 当项目中需要设计对应的提示词时，让其参考
> https://github.com/Piebald-AI/claude-code-system-prompts 项目中的提示词工程
> 是如何设计的。同时能够确保含有优秀的缓存命中，避免项目出现低缓存命中。

> 我的意思是：我们这个提示词框架要说清楚，当项目中设计一些提示词的设计的时候
> 参考 …这个是怎么设计的。

### Changes Overview

- Areas: `docs/rules/` (new file), `AGENTS.md` (routing + conventions),
  `docs/rules/docs-in-sync.md` (table), `docs/rules/README.md`,
  `docs/design-docs/api-portability.md`,
  `docs/product-specs/0009-skills.md`, `scripts/check-docs.sh`.
- Key actions:
  - New `docs/rules/prompt-design.md` carries two pieces of guidance:
    1. **The Study Reference.** When a slice introduces a new prompt
       surface (system preamble, tool-catalog rendering, skill body
       template, memory framing, deferred-tool index, approval-prompt
       template, anything an upstream model reads), the contributor
       **must consult**
       `https://github.com/Piebald-AI/claude-code-system-prompts` as
       the catalog of proven shapes — and record which patterns were
       adopted or rejected (with a reason) in the matching design
       doc.
    2. **Cache discipline.** The prompt builder must produce
       byte-identical preambles across iterations when underlying
       inputs are unchanged, or every iteration re-bills the full
       cached prefix. Hard invariants: no clocks in the cached
       sections; no request IDs or per-call counters; tool
       descriptions are pure functions of `ToolDef`; skill bodies
       live in their own section (not inline in the preamble); merge
       tiny sections to stay above the Anthropic 1024-token
       cache-block floor; `cache_version` bumps are explicit. The
       prompt is assembled stable → dynamic across seven named
       sections, with the cache breakpoint between (6) per-agent
       overlay and (7) conversation tail.
  - `AGENTS.md` "Module Routing" gains a `Prompts` row pointing at
    `prompt-design.md` and the three design / spec docs that own
    prompt-bearing surfaces. "Conventions At A Glance" gains a
    one-line "Prompt design" row that names the reference URL
    inline so the routing layer surfaces it directly.
  - `docs/rules/docs-in-sync.md` table gains a row "New / changed
    prompt surface" → update `prompt-design.md` (if a new invariant
    is introduced), `api-portability.md` (if cache layout shifts),
    and the design / spec doc owning the surface.
  - `docs/rules/README.md` lists `prompt-design.md` in the "Read
    These Before Touching Code" table so the rules index is honest.
  - `docs/design-docs/api-portability.md` "Caching" gets a one-line
    pointer to `prompt-design.md` as the canonical home for what
    content goes in which `CacheSection`; the design doc keeps the
    adapter-side mapping only.
  - `docs/product-specs/0009-skills.md` Scope (v1) notes that
    catalog rendering and skill-body placement follow the
    cache-section ordering in `prompt-design.md`.
  - `scripts/check-docs.sh` adds `docs/rules/prompt-design.md` to
    the required-files list.

### Design Intent

The project builds an LLM agent runtime; prompts are a load-bearing
engineering surface, not creative writing. Prompt engineering also
evolves incrementally with the features it serves — a single
authoritative rule today would be premature. The rule is therefore
scoped to **what is already designed** in `api-portability.md` (the
`CacheSection` model) and is explicitly marked as "rows are added
as prompt-bearing libraries land (agent loop → tool registry render
→ skill loader → memory framing → channel framing)".

The Piebald repo is named as the *catalog of prior art* rather than
copied into the repo: copying would freeze a snapshot whose
upstream evolves, and the value is in pattern recognition not
verbatim text. The URL is the only external reference cited; if it
ever moves or 404s, this rule must be fixed in the same PR that
discovers the breakage (Prime Directive).

Cache-hit invariants are stated hard because every byte of drift
in the cached prefix re-bills the entire preamble. The seven-section
ordering matches the design already in `api-portability.md` and
gives reviewers a single place to ask "which section does this
content belong to?"

### Files Modified

- `docs/rules/prompt-design.md` — new file.
- `docs/rules/README.md` — new row in "Read These Before Touching Code".
- `docs/rules/docs-in-sync.md` — new row in change-type table.
- `AGENTS.md` — new "Module Routing" row, new "Conventions At A
  Glance" row.
- `docs/design-docs/api-portability.md` — short pointer at the end
  of "Caching".
- `docs/product-specs/0009-skills.md` — short pointer in Scope (v1).
- `scripts/check-docs.sh` — required-file list.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/rules/prompt-design.md` — canonical rule.
- `docs/rules/README.md` — rules index now honest.
- `docs/rules/docs-in-sync.md` — change-type table extended.
- `AGENTS.md` — routing + conventions surfacing.
- `docs/design-docs/api-portability.md` — pointer back to canonical
  rule.
- `docs/product-specs/0009-skills.md` — pointer at v1 scope.
- This history entry —
  `docs/histories/2026-05/20260517-0130-prompt-design-rule.md`.
- `docs/STATUS.md` — `Last completed history` repointed.

### Validation

- Commands run: `make ci` (passes; new file is required,
  `docs-sync` accepts the rule's references, freshness gate green);
  markdown link rescan reports 0 broken local links.
- Tests added/changed: none — documentation only.
- Bench impact: none. A future
  `bench/oran-agent/prompt_cache_hit_rate.cpp` is named in the rule
  as the planned regression gate once `oran-agent` lands.
- Compile-budget delta: none.

### Follow-ups

- Issues to file: none.
- Tech-debt entry: none.
- Linked release note: none (pre-release; framework-only change).
- When the first stable preamble template lands in code, add
  `scripts/check-prompt-preamble` (grep, pattern on
  `check-banned-includes.sh`) and list it under "Mechanical
  Enforcement" in `docs-in-sync.md`.
- When `oran-agent` lands, ship the
  `bench/oran-agent/prompt_cache_hit_rate.cpp` scenario named in
  the rule.

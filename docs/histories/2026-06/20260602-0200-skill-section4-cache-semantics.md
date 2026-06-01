## [2026-06-02 02:00] | Task: Skill Section-4 Cache Semantics

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - this is the doc-only follow-up named by
  `docs/STATUS.md` after slice 143.

### User Query

Continue the prompt-runtime arc with a documentation-first pass before adding
more skill activation behavior.

### Changes Overview

- Areas: skill prompt semantics, prompt-cache docs, bootstrap runner docs,
  status tracking.
- Key actions: document that skill activation, deactivation, and expiration
  policy is prompt-boundary state for section 4; define deterministic inputs
  for byte-identical section-4 rendering; clarify that active-marker changes
  invalidate the cached prefix through section content, while rendering-rule
  changes require a `SectionVersions::skills_catalog` bump; and preserve the
  exact caller-supplied `skills_catalog` bypass for tests and embedders.

### Design Intent

Slice 143 exposed `skill::ActivationPolicy`, but expiration and deactivation
rules still needed cache semantics before code could safely extend the policy.
This slice records those invariants in the product and portability docs so the
next implementation can add explicit policy inputs without hidden clocks,
mid-turn section rewrites, or accidental skill-body movement into the cached
prefix.

### Files Modified

- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/agent-platform.md`
- `docs/design-docs/api-portability.md`
- `docs/design-docs/bootstrap-runtime.md`
- `docs/product-specs/0009-skills.md`
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md`
- `docs/rules/prompt-design.md`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - doc-only slice pointer plus next intended policy
  extension direction.
- `docs/ARCHITECTURE.md` - inventory rows now distinguish the shipped
  activation-policy resolver from the documented cache-semantics follow-up.
- `docs/design-docs/agent-platform.md` - prompt-boundary section-4 policy
  ownership and cache invalidation rules.
- `docs/design-docs/api-portability.md` - section-4 cache-version versus
  content-hash invalidation guidance.
- `docs/design-docs/bootstrap-runtime.md` - exact catalog bypass and
  prompt-boundary policy application.
- `docs/product-specs/0009-skills.md` - activation/deactivation/expiration
  cache semantics and acceptance criterion.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` - skill
  activation cache-boundary acceptance criterion.
- `docs/rules/prompt-design.md` - hard invariant for section-4 policy inputs.

### Validation

- Commands run:
  - `git diff --check`
- Tests added/changed: none - documentation-only semantics slice.
- Bench impact: none.
- Compile-budget delta: none.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: none - no runtime behavior changed.

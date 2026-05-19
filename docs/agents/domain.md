# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

This repo does **not** use the default `CONTEXT.md` + `docs/adr/` layout. It has its own
documentation framework — wherever a skill asks for "CONTEXT.md" or "the ADRs", consult the
mappings below instead.

## Before exploring, read these

Read in this order, stopping when you have enough to act:

1. **`docs/STATUS.md`** — one-screen project snapshot: current slice, last completed history, active exec-plan, open tech-debt. **Always read first.**
2. **`AGENTS.md`** (root) — the routing index. It tells you which design-doc / rule / product-spec applies to the area you're about to touch.
3. **`docs/ARCHITECTURE.md`** — target architecture map, library boundaries, binary inventory. The closest equivalent to a system-wide CONTEXT.md.
4. **`docs/design-docs/core-beliefs.md`** — non-negotiable operating principles. Read before proposing architectural change.
5. **`docs/design-docs/<area>.md`** — the design doc for the module the task touches (see the routing table in `AGENTS.md`).
6. **`docs/rules/critical-rules.md`** — non-negotiable C++/build constraints. Read before any code edit.
7. **`docs/rules/<area>.md`** — area-specific rules (compile-budget, error-handling, async-and-concurrency, code-style, etc.) per the `AGENTS.md` routing table.

## Skill-vocabulary → repo-equivalent

| Skill says…                       | In this repo, read…                                                                  |
| --------------------------------- | ------------------------------------------------------------------------------------ |
| `CONTEXT.md`                      | `docs/ARCHITECTURE.md` for system-wide, `docs/design-docs/<area>.md` for per-module  |
| Glossary / domain language        | The `AGENTS.md` "Conventions At A Glance" table and the relevant `docs/rules/*.md`   |
| `docs/adr/`                       | `docs/design-docs/` (architectural decisions) + `docs/rules/` (binding constraints)  |
| ADR-0007 (or any numbered ADR)    | The matching design-doc filename, e.g. `docs/design-docs/async-model.md`             |
| Past decisions / "why we do X"    | `docs/design-docs/core-beliefs.md`, `docs/histories/`, `docs/references/`            |

## Layout

Single-context repo. There is no `CONTEXT-MAP.md` and there are no per-area `CONTEXT.md` files.

```
/
├── AGENTS.md                      ← routing index (read first for any task)
├── docs/
│   ├── STATUS.md                  ← project snapshot
│   ├── ARCHITECTURE.md            ← system-wide architecture (≈ CONTEXT.md)
│   ├── design-docs/               ← per-area architecture (≈ context-scoped ADRs)
│   ├── rules/                     ← binding constraints (≈ ADRs that say "must")
│   ├── product-specs/             ← product surface
│   ├── histories/                 ← what was done and why
│   └── references/                ← external prior art and legacy audit
└── src/
```

## Use the project's vocabulary

When your output names a domain concept (issue title, refactor proposal, hypothesis, test name), use the term as defined in the relevant design-doc or rule. Don't drift to synonyms the docs explicitly avoid.

The "Conventions At A Glance" table in `AGENTS.md` is the canonical short list; each row links to the rule that owns the term.

If the concept you need isn't in any design-doc or rule yet, that's a signal — either you're inventing language the project doesn't use (reconsider) or there's a real gap (propose the doc edit alongside your change, per the Prime Directive in `AGENTS.md`).

## Flag design-doc / rule conflicts

If your output contradicts an existing design-doc or rule, surface it explicitly rather than silently overriding:

> _Contradicts `docs/design-docs/async-model.md` ("no `std::thread`") — but worth reopening because…_

Per the Prime Directive (`AGENTS.md`), proposing a rule edit comes **before** breaking the rule.

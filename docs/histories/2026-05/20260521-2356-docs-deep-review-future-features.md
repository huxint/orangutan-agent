## [2026-05-21 23:56] | Task: Land deep-review-2026-05-21 future-feature roadmap into docs framework

### Execution Context

- Agent: Claude (Opus 4.7)
- Base model: claude-opus-4-7[1m]
- Runtime: Claude Code CLI
- Linked plan: none (docs-only slice; per `docs/PLANS_GUIDE.md` "When NOT To Create A Plan")

### User Query

> 深度了解项目架构，然后根据 /tmp/orangutan-refactor-agent-tool-review-2026-05-21.md
> 把一些未来的特性和功能说明清楚，整理好写入提示词框架, 方便以后逐步实现.

### Changes Overview

- Areas: docs/product-specs, docs/design-docs, docs/exec-plans, top-level routing.
- Key actions:
  - Drafted three new product specs to give the second deep review's
    future features a permanent home (each follows the existing
    `user problem / scope v1 / v1.1 / v2 / out-of-scope / acceptance
    criteria / cross-refs / risks / validation` shape so future agents
    can ship from the spec alone).
  - Added forward-looking sections to two design docs and one agent
    platform doc so the specs cross-reference back to architectural
    invariants.
  - Logged the cleanup items that did *not* turn into a spec as a new
    tech-debt row, with one-line opens marked safe.
  - Updated routing (CLAUDE.md) and STATUS.md so the next agent finds
    the roadmap on the first read.

### Design Intent

The original deep-review document (`/tmp/...md`) is temporary, and the
existing tracker row already groups deep-review backlog items as small
unit-of-work cleanups. The *future-shape* sections of the review
(workspace policy, file-view system, parallel tool scheduler,
bounded-state inventory, file-search optimisation layers,
write/edit optimisation plan) are too large for a tracker row and
too opinionated to leave as a paragraph in a design doc — they are
*what the user gets*, not *how the layer works internally*, so they
belong in `product-specs/`.

Three specs were drafted because the three concerns are coherent on
their own and can ship independently:

- **0013 workspace + path policy** is the foundation; everything else
  composes on top of resolved canonical paths.
- **0011 file-view system** is the agent-facing correctness contract:
  range reads, fingerprints, `if_version`, bounded caches, line-offset
  index. It explicitly leans on 0013's resolver.
- **0012 tool scheduler + bounded state** is the concurrency contract:
  per-path locks, ordered results, `BoundedCache` primitive, observability.
  It lives in `oran-tool::Scheduler` pre-`oran-agent` and migrates into
  `oran-agent::ToolScheduler` in the slice that creates the lib.

Alternatives rejected:

- One umbrella roadmap doc — easier to write but breaks the
  spec/design-doc separation and is harder to grab an isolated v1
  criterion from.
- Inline everything into existing design-docs — loses the
  product-vs-implementation distinction the framework already enforces.

### Files Modified

- `docs/product-specs/0011-file-view-and-caching.md` (new)
- `docs/product-specs/0012-tool-scheduler-and-state.md` (new)
- `docs/product-specs/0013-workspace-and-path-policy.md` (new)
- `docs/product-specs/index.md`
- `docs/design-docs/tool-runtime.md`
- `docs/design-docs/io-runtime.md`
- `docs/design-docs/agent-platform.md`
- `docs/exec-plans/tech-debt-tracker.md`
- `CLAUDE.md`
- `docs/STATUS.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/product-specs/0011-…` / `0012-…` / `0013-…` — three new drafted specs.
- `docs/product-specs/index.md` — registered the new specs.
- `docs/design-docs/tool-runtime.md` — added "Workspace Handle",
  "Scheduler Boundary", "Output Shape v2" forward-looking sections
  pointing at the new specs.
- `docs/design-docs/io-runtime.md` — appended range-read v2, atomic-write
  durability mode, and `io::run_blocking` to "Future Slices".
- `docs/design-docs/agent-platform.md` — cross-cutting Cancellation and
  Backpressure bullets now cite the scheduler spec as the enforcement
  point.
- `docs/exec-plans/tech-debt-tracker.md` — new row capturing the six
  non-spec cleanup items from the review; existing deep-review row
  cross-references the new specs as the roadmap home for the
  *future-feature* (non-cleanup) work.
- `CLAUDE.md` — routing rows for tool scheduler, file-view, and
  workspace specs.
- `docs/STATUS.md` — "Next intended slice" now points at the v1
  acceptance criteria of the three new specs.

No production code touched in this slice. No `oran-tool` / `oran-io` /
`oran-bootstrap` shape changed; all spec contracts are forward-looking
and gated on future slices.

### Validation

- Commands run: none required beyond docs. `make ci` will run on PR; the
  `check-status-fresh` script needs this new history file to be the
  newest under `docs/histories/2026-05/`.
- Tests added/changed: none (no code).
- Bench impact: none (no code).
- Compile-budget delta: none (no code).

### Follow-ups

- Issues opened: none.
- Tech-debt entries: new row 2026-05-21 `review/deep-2026-05-21-followup`
  (six cleanup items).
- Linked release note: none (no user-visible behaviour).
- Next slice candidates per STATUS.md "Next intended slice":
  - Land 0013 v1 acceptance criterion #1 (traversal rejection) by
    introducing `tool::Workspace` + `DispatchContext::workspace` and
    migrating `file.read` first.
  - Land 0011 v1 acceptance criterion #4 (stale-edit detection) by
    adding `expected_version` to `file.edit` once 0013 has shipped at
    least one built-in.
  - Land 0012 v1 acceptance criterion #2 (ordered results) with a
    `tool::Scheduler` skeleton against fake tools; this is safe to ship
    pre-`oran-agent`.

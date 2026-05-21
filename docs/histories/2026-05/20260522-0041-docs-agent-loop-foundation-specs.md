## [2026-05-22 00:41] | Task: Land agent-loop-foundation roadmap as specs 0014–0018; stale-banner deep-review.md

### Execution Context

- Agent: Claude (Opus 4.7 1M)
- Base model: claude-opus-4-7[1m]
- Runtime: Claude Code CLI
- Linked plan: none (docs-only slice; per `docs/PLANS_GUIDE.md` "When NOT To Create A Plan")

### User Query

> 先提交你当前的框架文档, 然后继续根据 /tmp/orangutan-agent-loop-foundation-2026-05-21.md
> 进行更新提示词框架, 一样的把特性和功能整理好写入提示词框架. 最后检查, 确保提示词框架
> 逻辑闭环. 然后再提交和push. 当然, 有些过时的老文档或者重复冗余的文档可以更新或者删除.

Follow-up to the previous slice (specs 0011/0012/0013) which committed
as `57be783`.

### Changes Overview

- Areas: docs/product-specs, docs/design-docs, docs/exec-plans, root
  `orangutan-deep-review.md`, CLAUDE.md routing, STATUS.md.
- Key actions:
  - Drafted five new product specs covering every non-scheduler concern
    of `/tmp/orangutan-agent-loop-foundation-2026-05-21.md`. (§1 of the
    foundation doc was already covered by spec 0012; this slice extended
    0012 with the foundation doc's classify-by-resource detail and the
    cross-refs into 0014/0015/0017/0018.)
  - Added a stale banner to `orangutan-deep-review.md` (slice-30
    review artifact) pointing readers at STATUS.md, the tracker, and
    specs 0011–0018 instead. The body is preserved because the tracker
    cross-references its `§4.x.y` anchors.
  - Closed the loop: every recommendation from both source docs
    resolves to a spec / design-doc / rule / tracker row; every spec's
    cross-references resolve to a real file; the suggested build order
    in STATUS.md is a valid dependency DAG.
  - Refreshed two tracker rows (2026-05-18 hook bus, 2026-05-17 prompt
    bench + grep) so their "Planned Follow-Up" cells now point at the
    spec that defines what "done" looks like — no row was removed
    early; rows close in the same slice that ships the spec's v1.

### Design Intent

The agent-loop-foundation note overlapped heavily with §Parallel tool
calls + §Bounded runtime state of the earlier deep review (already
absorbed into spec 0012 in the previous slice), but added five distinct
concerns that did not have a spec home:

- **0014 structured tool output** — `{text, is_error}` is too small for
  the planned runtime. Spec defines the v2 envelope, byte caps, hook
  redaction, adapter mapping, and the *one-by-one* migration policy so
  v1 ships without forcing every existing built-in to change at once.
- **0015 blocking hook decisions** — gives the 2026-05-18 hook
  tech-debt row a product-side home with acceptance criteria for the
  four-decision `HookDecisionKind` and the seven-step canonical
  dispatch order. Closes the row when v1 ships.
- **0016 prompt + tool-catalog cache** — turns `rules/prompt-design.md`
  invariants into a library (`oran-prompt`) with deterministic
  renderers, deferred-tool index, `tool.search`, and the prompt-cache
  stability bench tracked in the 2026-05-17 prompt tech-debt rows.
- **0017 fake-provider-first agent loop** — the load-bearing
  sequencing claim from the foundation note. Ships
  `provider::FakeProvider` + ten scripted scenarios before any vendor
  adapter, so the loop's contract is testable offline. v1.1 brings the
  Anthropic adapter with *zero loop changes*.
- **0018 first-loop observability** — per-turn `trace_turns` row +
  `parent_turn_id` cause-chain join. Synchronous insert with the
  user-visible response so the row is durable before the agent
  answers. Schema is intentionally minimal (eleven columns +
  `context_json` grab-bag); typed columns graduate from `context_json`
  only after a third consumer reads them.

Alternatives rejected:

- **Inline §2/§4/§5/§7/§8 into existing design-docs.** Tried in the
  routing decision; rejected because spec-vs-design-doc separation
  exists for a reason — design-docs say *how the layer works
  internally*, specs say *what the user gets, how it's tested, in what
  order*. The foundation note is firmly the latter.
- **Delete `orangutan-deep-review.md`.** Considered per the user's
  prompt about stale docs. Rejected because the tracker row's
  `§4.x.y` cross-references would break, and because the slice-30
  review's executive summary is useful historical context. A stale
  banner that redirects future readers is the lower-risk fix.
- **Replace `docs/rules/prompt-design.md` with spec 0016.** Rejected
  because the rule is the *invariant layer* (review-blocking); the
  spec is the *deliverable layer*. The rule outlives any one library.

### Files Modified

- `docs/product-specs/0014-structured-tool-output.md` (new)
- `docs/product-specs/0015-blocking-hook-decisions.md` (new)
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` (new)
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` (new)
- `docs/product-specs/0018-first-loop-observability.md` (new)
- `docs/product-specs/0012-tool-scheduler-and-state.md` (extended:
  classify-by-resource list; cross-refs into 0014/0015/0017/0018)
- `docs/product-specs/index.md` (registered new specs)
- `docs/design-docs/agent-platform.md` (Goals For The First 12 Months
  point #1 now cross-refs 0014/0015/0016/0017/0018)
- `docs/exec-plans/tech-debt-tracker.md` (2026-05-18 hook row → spec
  0015 v1; 2026-05-17 prompt rows → spec 0016 v1)
- `CLAUDE.md` (routing rows for ReAct loop, tools/hooks/permissions,
  prompts/catalog, reliability/observability)
- `docs/STATUS.md` (Next intended slice now names all eight specs +
  the build-order DAG)
- `orangutan-deep-review.md` (stale banner at the top)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/product-specs/0014…/0018-…` — five new drafted specs.
- `docs/product-specs/0012-…` — extended with cross-refs into the new
  specs and the classify-by-resource list from the foundation note.
- `docs/product-specs/index.md` — registered the new specs.
- `docs/design-docs/agent-platform.md` — MVP runtime goal updated to
  reference the v1 sequencing in 0017 + the sibling specs.
- `docs/exec-plans/tech-debt-tracker.md` — two existing rows now name
  the spec that defines their "done" state.
- `CLAUDE.md` — routing rows updated; no new module rows because
  prompts/observability lived under existing rows.
- `docs/STATUS.md` — Next intended slice now names every new spec +
  the recommended build order (0013 → 0011 + 0012 → 0014 → 0016 →
  0017 → 0015 → 0018).
- `orangutan-deep-review.md` — stale banner with pointers to STATUS,
  tracker, and specs 0011–0018.

No production code touched in this slice. All spec contracts are
forward-looking and gated on future slices.

### Closed-Loop Verification

Every recommendation from
`/tmp/orangutan-refactor-agent-tool-review-2026-05-21.md` and
`/tmp/orangutan-agent-loop-foundation-2026-05-21.md` terminates at a
spec / design-doc / rule / tracker row. Every spec's `Design Doc
Cross-References` block resolves to an existing file. The build order
in `STATUS.md` is a valid dependency DAG: 0013 has no spec deps; 0011
+ 0012 depend on 0013; 0014 has no spec deps for v1; 0016 depends on
0014 (v1.1); 0017 depends on 0016 + 0014; 0015 depends on 0012 + 0017
(the agent loop is consumer #1 of `publish_blocking`); 0018 depends on
0017 (the loop owns `TurnId`). No spec depends on an undefined
primitive; no recommendation is orphaned.

### Validation

- `make ci` ran twice during the slice — both passes. Doc scaffold,
  hygiene, docs-sync, STATUS freshness (against the previous slice's
  history entry), dep layering, and action pinning all green.
- Tests added/changed: none (no code).
- Bench impact: none (no code).
- Compile-budget delta: none (no code).

### Follow-ups

- Issues opened: none.
- Tech-debt entries: no new rows. Two existing rows (2026-05-18 hook,
  2026-05-17 prompt × 2) point at the specs that will close them.
- Next slice candidates per STATUS.md "Next intended slice":
  - Spec 0013 v1 #1 (traversal rejection) — safest open; lands
    `tool::Workspace` + migrates `file.read` first.
  - Spec 0017 v1 #1 (single-text fake-provider turn) — opens up the
    agent-loop work; can run in parallel with 0013 because spec 0017
    v1 does not yet route through Workspace.
  - Spec 0014 v1 #1 (text-only round-trip) — safe envelope migration;
    every built-in keeps current behaviour via `Output::text_only(...)`.

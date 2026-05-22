# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now. Update in the same commit as
> the history entry that moves it — see
> [`rules/docs-in-sync.md`](rules/docs-in-sync.md).

## Snapshot

- **Slice:** 35 (`xmake run orangutan` reports slice 35)
- **Last completed history:**
  [`histories/2026-05/20260522-1408-tool-registry-schema-validation.md`](histories/2026-05/20260522-1408-tool-registry-schema-validation.md)
- **Active exec-plan:** none — current slice intent fits inside the
  `Next intended slice` bullet below; see
  [`PLANS_GUIDE.md`](PLANS_GUIDE.md) "When NOT To Create A Plan".
  When `active/` is non-empty, link the file path here instead.
- **Next intended slice:** TBD — slice 35 closed the deep-review
  `Registry::add` schema-validation P0 item: tool registration now
  rejects empty `input_schema_json`, unparseable JSON, non-object
  top-level schemas, and malformed common JSON Schema keywords
  (`type`, `properties`, `required`, `additionalProperties`, `enum`,
  `minimum`, `maximum`) before mutating the registry. Heavy JSON
  parsing lives in a separate `src/oran-tool/schema_validation.cpp`
  TU so `registry.cpp` stays focused on dispatch. Remaining
  deep-review P0 surface in `exec-plans/tech-debt-tracker.md`
  under the `deep-review-2026-05-21` group: transparent hashing on
  `Registry::entries_` to remove the per-dispatch lookup allocation.
  The next-likely tool-side slices remain
  unchanged: the first provider adapter (Anthropic Messages —
  multi-slice, needs an exec plan and `oran-http` + libcurl
  wiring first), blocking hook semantics with veto (still gated
  on `oran-agent`), or wiring `check-compile-budget.sh` into
  `scripts/ci.sh` (the slice-28 tech-debt entry covers the
  preconditions). The current `file.delete` and `directory.list`
  shapes are expected to be re-shaped in a later refactor: one
  unified delete tool covering both files and folders, and a
  recursive whole-project list (not just single-level children).
  Future built-in slices should not double down on per-kind splits
  like `directory.remove` or single-level enumeration. The
  *future-feature* roadmap surfaced by the second 2026-05-21 deep
  review (Refactor-Agent Tool Review) and the companion
  agent-loop foundation note now lives in eight new product specs:
  [`0011-file-view-and-caching.md`](product-specs/0011-file-view-and-caching.md)
  (range reads, fingerprints, `if_version`, bounded caches),
  [`0012-tool-scheduler-and-state.md`](product-specs/0012-tool-scheduler-and-state.md)
  (parallel tool dispatch, per-path locks, `BoundedCache`, index
  caches),
  [`0013-workspace-and-path-policy.md`](product-specs/0013-workspace-and-path-policy.md)
  (workspace confinement, symlink policy, override roots),
  [`0014-structured-tool-output.md`](product-specs/0014-structured-tool-output.md)
  (`ToolOutput { text, data, attachments, usage, is_error }`,
  byte caps, hook redaction),
  [`0015-blocking-hook-decisions.md`](product-specs/0015-blocking-hook-decisions.md)
  (`publish_blocking`, `HookDecisionKind`, seven-step canonical
  dispatch order; closes the 2026-05-18 hook tech-debt row when v1
  ships),
  [`0016-prompt-and-tool-catalog-cache.md`](product-specs/0016-prompt-and-tool-catalog-cache.md)
  (`oran-prompt`, deterministic tool-block renderer, deferred-tool
  index, `tool.search`, prompt-cache stability bench),
  [`0017-fake-provider-first-agent-loop.md`](product-specs/0017-fake-provider-first-agent-loop.md)
  (`provider::FakeProvider`, ten scripted scenarios, agent loop
  ships against the fake **before** the first real adapter), and
  [`0018-first-loop-observability.md`](product-specs/0018-first-loop-observability.md)
  (`oran-storage::TraceRepository`, per-turn `trace_turns` row,
  cause-chain join via `parent_turn_id`). The recommended build
  order matches the spec dependency graph: 0013 → 0011 + 0012 → 0014
  → 0016 → 0017 → 0015 → 0018 (0018 can land any time after 0017
  since the schema is additive). Pick a v1 acceptance criterion from
  any of these as the next slice's charter.

## Library Health

Lifted from [`QUALITY_SCORE.md`](QUALITY_SCORE.md). `STATUS.md` summarizes;
`QUALITY_SCORE.md` explains.

| Score | Areas |
| ----- | ----- |
| **A** | *(none yet — pre-v1)* |
| **B** | Architecture docs, Build system, Async model, Security defaults, Supply chain |
| **C** | Compile-time discipline, Tests, Benches, IO, Storage, Config, Bootstrap, Provider system, Tool registry, Memory tiers, Permissions, Hooks, Channels, Orchestration, Automation, Web UI, CLI, Static analysis |
| **D** | Skills, Observability |

## Latest Library Surfaces

- `oran-core`: 54 cases / 370 assertions.
- `oran-async`: 9 cases / 43 assertions.
- `oran-io`: 16 cases / 70 assertions.
- `oran-storage`: 60 cases / 706 assertions.
- `oran-config`: 19 cases / 148 assertions.
- `oran-permission`: 83 cases / 379 assertions.
- `oran-hook`: 15 cases / 97 assertions.
- `oran-tool`: 94 cases / 760 assertions.
- `oran-cli`: 5 cases / 30 assertions.
- `oran-bootstrap`: 44 cases / 140 assertions.

## Open Tech-Debt Rows

Lifted from [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md).
Closed entries do *not* live here — the tracker is canonical.

- 2026-05-21 — Deep-review backlog: the stale root review artifact was
  deleted after its actionable findings were absorbed into the tracker and
  specs 0011-0018. Slices 31-35 closed the rank-0 items plus the first P0
  follow-ups; remaining follow-ups are grouped P0/P1/P2/P3 in the tracker.
- 2026-05-20 — `scripts/check-compile-budget.sh` exists and works (slice 28)
  but is not wired into `scripts/ci.sh`. Gated on CI provisioning xmake on
  the documented reference hardware (8-core / NVMe / native Linux);
  otherwise the gate fires on environmental drift, not real regressions.
- 2026-05-18 — Hook bus dispatch is advisory-only (slice 22); blocking
  semantics with veto for `tool_before` / `memory_*_before` /
  `permission_ask_rendered` are deferred to a follow-up slice.
- 2026-05-17 — `file.search` does not yet ship ripgrep-class optimisations
  (mmap, extension-based binary skip, `.gitignore`, multi-threaded walk).
  Adequate at slice 20 (~27 µs / 4-file tree) but 3-10× slower than a tuned
  scanner on repo-scale inputs. Re-bench once `oran-agent` produces a real
  workload measurement.
- 2026-05-17 — `scripts/check-prompt-preamble` static grep promised in
  `rules/prompt-design.md` not yet implemented (waits on first stable
  preamble template in `oran-agent`).
- 2026-05-17 — `bench/oran-agent/prompt_cache_hit_rate.cpp` regression
  scenario promised in `rules/prompt-design.md` not yet implemented
  (waits on `oran-agent` slice 1).
- 2026-05-14 — Generated `docs/generated/config.schema.json` not yet
  implemented.
- 2026-05-14 — bench A-vs-B scenarios listed in
  `bench/<lib>/README.md` are placeholders.
- 2026-05-14 — Frontend stack choice (Preact vs. plain JS) not yet
  decided.

## How To Update

1. The slice that lands a behavior change writes its history file.
2. The **same commit** updates this file: bump `Slice`, point
   `Last completed history` at the new file, refresh
   `Active exec-plan` (path or `none` + reason — see
   [`PLANS_GUIDE.md`](PLANS_GUIDE.md) "When NOT To Create A Plan"),
   refresh the test/assertion counts in "Latest Library Surfaces",
   and re-sync the tech-debt list from
   `exec-plans/tech-debt-tracker.md`.
3. `scripts/check-status-fresh.sh` fails the build if `STATUS.md`'s
   `Last completed history` pointer is older than the newest file under
   `docs/histories/`.

## See Also

- [`QUALITY_SCORE.md`](QUALITY_SCORE.md) — the per-area scoring rubric.
- [`releases/feature-release-notes.md`](releases/feature-release-notes.md)
  — chronological user-visible change log.
- [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md)
  — open debt rows.

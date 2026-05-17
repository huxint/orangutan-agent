# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now. Update in the same commit as
> the history entry that moves it — see
> [`rules/docs-in-sync.md`](rules/docs-in-sync.md).

## Snapshot

- **Slice:** 19 (`xmake run orangutan` reports slice 19)
- **Last completed history:**
  [`histories/2026-05/20260517-2300-oran-tool-file-edit.md`](histories/2026-05/20260517-2300-oran-tool-file-edit.md)
- **Active exec-plan:** none — current slice intent fits inside the
  `Next intended slice` bullet below; see
  [`PLANS_GUIDE.md`](PLANS_GUIDE.md) "When NOT To Create A Plan".
  When `active/` is non-empty, link the file path here instead.
- **Next intended slice:** TBD — choose from the QUALITY_SCORE "Next Step"
  column. Most likely candidates: the remaining file built-in
  (`file.search`) on top of the slice-17/18/19 `tool::Registry`;
  the approval-broker flow that replaces the `Verdict::ask` short-circuit
  in `Registry::dispatch`; the first provider adapter
  (Anthropic Messages); signal-aware shutdown for `bootstrap::run`.

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

- `oran-core`: 54 cases / 366 assertions.
- `oran-async`: 8 cases / 38 assertions.
- `oran-io`: 8 cases / 33 assertions.
- `oran-storage`: 60 cases / 702 assertions.
- `oran-config`: 19 cases / 148 assertions.
- `oran-permission`: 83 cases / 379 assertions.
- `oran-tool`: 31 cases / 227 assertions.
- `oran-cli`: 5 cases / 30 assertions.
- `oran-bootstrap`: 34 cases / 123 assertions.

## Open Tech-Debt Rows

Lifted from [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md).
Closed entries do *not* live here — the tracker is canonical.

- 2026-05-17 — `scripts/check-prompt-preamble` static grep promised in
  `rules/prompt-design.md` not yet implemented (waits on first stable
  preamble template in `oran-agent`).
- 2026-05-17 — `bench/oran-agent/prompt_cache_hit_rate.cpp` regression
  scenario promised in `rules/prompt-design.md` not yet implemented
  (waits on `oran-agent` slice 1).
- 2026-05-14 — Build-skeleton scripts referenced from rules but not yet
  implemented (`check-deps.sh`, `check-includes.sh`, `measure-tu.sh`,
  `check-compile-budget.sh`).
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

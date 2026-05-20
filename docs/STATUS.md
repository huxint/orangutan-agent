# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now. Update in the same commit as
> the history entry that moves it — see
> [`rules/docs-in-sync.md`](rules/docs-in-sync.md).

## Snapshot

- **Slice:** 25 (`xmake run orangutan` reports slice 25)
- **Last completed history:**
  [`histories/2026-05/20260520-1900-tool-lifecycle-events.md`](histories/2026-05/20260520-1900-tool-lifecycle-events.md)
- **Active exec-plan:** none — current slice intent fits inside the
  `Next intended slice` bullet below; see
  [`PLANS_GUIDE.md`](PLANS_GUIDE.md) "When NOT To Create A Plan".
  When `active/` is non-empty, link the file path here instead.
- **Next intended slice:** TBD — choose from the QUALITY_SCORE "Next Step"
  column. Slice 25 closed the remaining tool-lifecycle event row, so the
  only tool-side tech-debt left depends on `oran-agent`. Most likely
  candidates now: the first provider adapter (Anthropic Messages) —
  likely needs an exec plan since transport + protocol + execution +
  first integration test span multiple slices (and `oran-http` +
  libcurl wiring must land first); blocking hook semantics with veto
  (`tool_before` rewrite/short-circuit + the matching
  `permission_ask_rendered` blocking flow is one slice, but its first
  consumer lives in the not-yet-existing `oran-agent` library); or the
  first build-skeleton scripts (`check-deps.sh`, `check-includes.sh`,
  `measure-tu.sh`, `check-compile-budget.sh`) — currently stubs that no
  longer have a code-side blocker now that the library set has
  stabilised.

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
- `oran-hook`: 14 cases / 79 assertions.
- `oran-tool`: 74 cases / 599 assertions.
- `oran-cli`: 5 cases / 30 assertions.
- `oran-bootstrap`: 44 cases / 140 assertions.

## Open Tech-Debt Rows

Lifted from [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md).
Closed entries do *not* live here — the tracker is canonical.

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

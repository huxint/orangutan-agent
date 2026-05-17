# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now. Update in the same commit as
> the history entry that moves it — see
> [`rules/docs-in-sync.md`](rules/docs-in-sync.md).

## Snapshot

- **Slice:** 12 (`xmake run orangutan` reports slice 12)
- **Last completed history:**
  [`histories/2026-05/20260517-1100-audit-pipeline-and-bootstrap.md`](histories/2026-05/20260517-1100-audit-pipeline-and-bootstrap.md)
- **Active exec-plan:** none (`docs/exec-plans/active/` is empty)
- **Next intended slice:** TBD — choose from the QUALITY_SCORE "Next Step"
  column. Most likely candidates: `permission::AuditSink`
  interface that records allow / deny / ask / approved /
  rejected decisions through `storage::AuditRepository`
  (closes `0008-permissions.md` criterion 1); first tool-registry
  built-ins (`file.read`, `file.write`, `file.edit`,
  `file.search`); wire the `ApprovalBroker` into bootstrap so
  the upcoming agent loop inherits a per-process broker handle.

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
- `oran-permission`: 73 cases / 296 assertions.
- `oran-cli`: 5 cases / 30 assertions.
- `oran-bootstrap`: 8 cases / 34 assertions.

## Open Tech-Debt Rows

Lifted from [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md).
Closed entries do *not* live here — the tracker is canonical.

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
   `Last completed history` at the new file, refresh the test/assertion
   counts in "Latest Library Surfaces", and re-sync the tech-debt list
   from `exec-plans/tech-debt-tracker.md`.
3. `scripts/check-status-fresh.sh` fails the build if `STATUS.md`'s
   `Last completed history` pointer is older than the newest file under
   `docs/histories/`.

## See Also

- [`QUALITY_SCORE.md`](QUALITY_SCORE.md) — the per-area scoring rubric.
- [`releases/feature-release-notes.md`](releases/feature-release-notes.md)
  — chronological user-visible change log.
- [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md)
  — open debt rows.

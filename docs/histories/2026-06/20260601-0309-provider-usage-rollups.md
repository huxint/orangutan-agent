## [2026-06-01 03:09] | Task: slice 127 — provider usage rollups

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local shell, xmake release`
- Linked plan: none — single-slice follow-up selected from `docs/STATUS.md`

### User Query

Continue the doc-first architecture/progress pass, keep the execution plan moving, and
implement the next coherent runtime slice only after reading the relevant docs.

### Changes Overview

- Areas: `oran-storage`, trace observability docs.
- Key actions:
  - Added `storage::ProviderUsageRollup` and
    `ListProviderUsageRollupsOptions`.
  - Added `TraceRepository::list_provider_usage_rollups`, a read-only aggregate
    over existing `trace_turns` rows grouped by UTC day, agent key, route
    profile, and route model.
  - Covered aggregation, filters, ordering, limits, and invalid limit handling in
    `test-storage`.
  - Bumped the binary slice tag to `2.0.0-slice127`.

### Design Intent

Provider usage/cost rollups were the closest unfinished observability follow-up after
slice 126's provider lifecycle hooks. The trace table already stores
input/output/cache tokens and a provider-supplied `cost_estimate_usd`, so this slice
adds the storage-owned aggregate read first instead of inventing profile pricing in
the wrong layer.

The tradeoff is explicit: `TraceRepository` can now answer per-day usage questions
from durable trace data, but it only sums cost estimates that already exist on rows.
Computing spend from `profiles.<name>` pricing remains a separate config/provider
slice because the typed config parser does not yet accept cost fields.

### Files Modified

- `include/oran/storage/trace_repository.hpp`
- `src/oran-storage/trace_repository.cpp`
- `tests/storage/test_trace_repository.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/QUALITY_SCORE.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/api-portability.md`
- `docs/design-docs/storage-runtime.md`
- `docs/product-specs/0018-first-loop-observability.md`
- `docs/releases/feature-release-notes.md`
- `docs/histories/2026-06/20260601-0309-provider-usage-rollups.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 127 snapshot, last-history pointer, next-slice routing,
  and focused validation count.
- `docs/QUALITY_SCORE.md` — storage/test counts plus provider-cost follow-up text.
- `docs/ARCHITECTURE.md` — storage/provider inventory updates for storage-owned
  rollups and the hook-free provider boundary.
- `docs/design-docs/api-portability.md` — usage aggregation status and remaining
  profile-priced cost boundary.
- `docs/design-docs/storage-runtime.md` — `TraceRepository` aggregate-read surface.
- `docs/product-specs/0018-first-loop-observability.md` — AC6 status and repository
  rollup behavior.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build test-storage`
  - `xmake run test-storage` — 73 cases / 938 assertions
  - `xmake build orangutan`
  - `xmake run orangutan -- --help` — reports `orangutan v2.0.0-slice127`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - Trace repository usage rollup grouping by UTC day/profile/model.
  - Agent/profile/model filters and positive limit behavior.
  - Invalid zero-limit rejection for usage rollup listing.
- Bench impact:
  - None; no bench target changed.
- Compile-budget delta:
  - Small `oran-storage` implementation/header delta; no budget file change.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
  (`provider-usage-rollups`).

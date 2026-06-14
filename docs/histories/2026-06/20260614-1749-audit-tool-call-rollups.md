## [2026-06-14 17:49] | Task: Audit tool-call rollups

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none; this is a small unblocked spec-0018 Observability v1.1
  slice.

### User Query

> Continue iterating the project slice-by-slice after reading the development
> docs, choose the most valuable current slice, keep docs synchronized with
> reality, and close the slice with a detailed conventional commit.

### Changes Overview

- Areas: `oran-storage`, audit migrations, Observability docs.
- Key actions: added audit DB migration version 5 with the
  `audit_tool_call_rollups` SQL view; exposed
  `AuditRepository::list_tool_call_rollups(...)` with parent-turn, tool-name,
  and limit filtering; pinned migration/view/read semantics in `test-storage`;
  bumped the binary slice tag to `2.0.0-slice243`.

### Design Intent

Spec 0018 v1.1 asks for a per-turn tool-call rollup over joined audit rows. The
slice keeps that as a derived storage read instead of adding trace columns: the
source of truth remains `audit_events`, while the SQL view groups
permission-decision and sibling `hook_publish` rows by `parent_turn_id` and
`tool_name`. The implementation reports decision/hook counts, permitted/blocked
decision counts, and optional latency samples from valid
`metadata_json.usage.wall_time_ms`; it deliberately does not invent handler
failure-rate data because failed handler exits are not durable audit rows today.

### Files Modified

- `include/oran/storage/audit_repository.hpp`
- `src/oran-storage/audit_repository.cpp`
- `src/oran-storage/migration_assets.cpp`
- `src/oran-storage/migrations/audit/0005-audit-tool-call-rollups.sql`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/storage/test_audit_repository.cpp`
- `tests/storage/test_trace_repository.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped slice/history, recorded the rollup boundary and
  storage test counts, and selected the next unblocked Observability slice.
- `docs/ROADMAP.md` - advanced the Observability frontier to slice 243.
- `docs/product-specs/0018-first-loop-observability.md` - marked the v1.1
  tool-call rollup shipped and added the acceptance evidence.
- `docs/product-specs/index.md` - refreshed the spec-0018 status summary.
- `docs/design-docs/storage-runtime.md` - documented audit DB version 5, the
  view semantics, and the new repository API.
- `docs/ARCHITECTURE.md` - updated the `oran-storage` inventory.
- `docs/QUALITY_SCORE.md` - refreshed storage counts and Observability status.
- `docs/RELIABILITY.md` - documented the SQLite-native rollup as a read-side
  inspection primitive.
- `docs/releases/feature-release-notes.md` - added the user-visible release
  note.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-storage`
  - `build/linux/x86_64/release/test-storage "[audit_repository]" --reporter=console --verbosity=normal`
  - `build/linux/x86_64/release/test-storage --reporter=console --verbosity=normal`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap --reporter=console --verbosity=normal`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
  - `NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost build/linux/x86_64/release/test-http --reporter=console --verbosity=normal`
  - `NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost xmake test`
- Tests added/changed: storage migration tests now expect audit/trace migration
  version 5 and verify the `audit_tool_call_rollups` view; audit repository
  tests cover rollup aggregation, filters, limits, invalid JSON, missing turns,
  and malformed options.
- Environment note: an unqualified `xmake test` first failed only in
  `test-http/default` because this shell's proxy variables sent
  `ws://127.0.0.1:1/` through `HTTP_PROXY`; `NO_PROXY=127.0.0.1,localhost`
  made the existing loopback no-listener test pass and the default 18-bucket
  suite then passed.
- Bench impact: none; the slice adds a read-side SQL view/API and no hot-path
  writes.
- Compile-budget delta: one small storage migration embed and one existing
  `AuditRepository` implementation extension; no new public dependency edge.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`.

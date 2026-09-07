# Runtime Storage Contract

## Objective

Keep persistence focused on the configured provider/tool/session loop: ordered
conversation suffixes, scoped permission audits and correlated turn records.
Remove application-management APIs with no runtime consumer while preserving
existing databases and the reads needed to verify those records.

## Current Contracts And Evidence

`AgentSession` uses `Store::load_tail` and `Store::append_all` for continuation.
`StorageAuditSink` appends decisions and enriches their metadata. `Loop` appends
redacted turn records. Their repositories also expose readback for integration
checks. The following surfaces have only tests or forwarding wrappers as callers.

| Severity | File:Line | Check | Evidence and consequence | Fix | Outcome |
| --- | --- | --- | --- | --- | --- |
| Moderate | `include/oran/memory/session.hpp:31` | C/E/H | Session listings and skill state duplicate storage types and validation without a session-runner consumer. | Remove the memory and storage APIs, SQL and dedicated fixtures. | Scoped. |
| Moderate | `include/oran/storage/audit_repository.hpp:145` | C/E | Tool rollup queries maintain a reporting interface without an application consumer. | Remove the C++ aggregation surface; preserve its database view. | Scoped. |
| Moderate | `include/oran/storage/trace_repository.hpp:86` | C/E | Usage reports and retention deletion have no runtime caller or configured retention policy. | Remove their C++ types, queries and tests; retain turn persistence and readback. | Scoped. |

Contracts: [storage](../../design-docs/storage-runtime.md),
[memory](../../design-docs/memory-system.md),
[composition](../../design-docs/bootstrap-runtime.md),
[permissions](../../design-docs/permissions-and-hooks.md).

## Smallest Complete Slice

- Remove session skill activation APIs and session-listing APIs from both layers.
- Keep atomic message appends, ordered/full and bounded-tail reads, and keyed
  session metadata readback.
- Remove tool-call and provider-usage aggregation APIs and trace purging.
- Keep audit/trace writers, metadata enrichment and bounded record queries.
- Preserve migration files, schema versions, tables, views and stored rows.
- Delete obsolete tests and update the owning contracts and handoff.

The change introduces no dependency or runtime option. It reduces public header
and translation-unit work. Audit/trace executor placement remains the separate
storage-execution obligation in [live debt](../tech-debt-tracker.md).

## Risks And Verification

Removing an API must not erase its database history. Exercise reopen/migration
and normal runtime writes with pre-existing session metadata, skill rows, audits
and traces; verify that retained tables/views and saved values survive. Preserve
the existing atomic rollback, serialization, ordering, scope and audit-correlation
regressions. Prove each added or behaviorally rewritten test with a deliberate
fault in an isolated copy, followed by green verification.

Run affected storage/memory/bootstrap tests, then the normal release build,
all test targets and `make ci`. Build and run storage/memory benchmarks to check
their retained API usage; no performance claim is required for these deletions.
Inspect remaining symbol references and the full diff before committing.

## Completion

- [x] Trace production callers and scope the removed contracts.
- [ ] Remove the unused APIs and their implementation/test-only support.
- [ ] Verify database preservation and retained runtime behavior.
- [ ] Update owning documents, complete release gates and commit the slice.

Delete this plan once the current contracts and verification are recorded in
their owning documents. Git retains the implementation history.

# Scoped Long-Term Memory Recovery

## Objective

Complete the persistence prerequisite in live debt: explicitly back up long-term
memory and import one source scope into one empty destination scope without
changing stored record formats or overwriting user data.

## Scope And Contracts

- Add host maintenance APIs to `memory::longterm::Fts5Backend`, following the
  existing session backup/import boundary. Hosts supply paths and scope authority
  and run these operations on the blocking executor; model tools do not expose them.
- Back up the whole database with the existing verified SQLite snapshot API.
- Import current schema version 1 from a read-only, pinned source snapshot.
  Preserve IDs, links, kinds, timestamps, JSON metadata and shadow rows exactly;
  only the explicit destination scope changes. Rebuild FTS from imported records.
- Reject occupied destination scopes, including orphan FTS rows, missing source
  scopes, unsupported schemas and invalid selectors. SQL failures and observed
  cancellation roll back the destination transaction.
- No merging, automatic migration, record-ID remapping, provenance schema,
  permission grants or host CLI in this slice.

Owners: [memory](../../design-docs/memory-system.md),
[storage](../../design-docs/storage-runtime.md),
[async rules](../../rules/async-and-concurrency.md).

## Implementation And Risks

Use a separate implementation translation unit with narrow includes and existing
storage primitives. Stream rows without collecting an unbounded scope in memory.
Do not copy a potentially stale source search index. Preserve raw stored metadata
rather than normalizing it through current record validation. No new dependency
or compile-budget increase is planned.

This slice exceeds six files because public declarations, implementation,
regressions and the existing contract/debt owners must change together.

## Verification And Completion

- Test backup/reopen, explicit remapping, full record preservation, scope isolation,
  search reconstruction and unchanged source data/migration history.
- Test destination collisions, orphan indexes, missing/invalid sources, rollback
  after a late write failure and cancellation while awaiting writer admission.
- Run affected memory tests, full release build/tests and `make ci`; inspect the
  diff and commit the complete slice.
- Update contract owners and remove the completed persistence debt and this plan.

## Progress

- [x] Read current contracts and select the persistence prerequisite.
- [ ] Implement and verify recovery APIs.
- [ ] Update owning contracts, run release gates and commit.

# Scoped Lexical Memory

## Objective

Keep one long-term memory path for the configured agent loop: dispatch an
authorized, host-scoped request to the lexical backend, render explicit result
values and retain the session's existing persistence and cleanup boundaries.
Remove dormant retrieval modes and the resource-free runtime facade.

## Current Contracts

`AgentSession` binds `MemoryRecall`, `MemoryRemember` and `MemoryForget` through
`Registry::dispatch`. The dispatch boundary owns schema validation, permission
decisions, approvals, hooks and output limits. The binding captures the host's
scope and runs SQLite work on the blocking executor. `RuntimeAssembly` owns the
pool and `Fts5Backend`; sessions and child calls borrow them until tool cleanup
finishes.

The executable constructs no vector backend, embedding producer or hybrid
runtime. Their consumers are dedicated tests and benchmarks. Backend decay has
no production caller. `longterm::Runtime` owns no resource; its search delegates
to the backend, and recall adds read timestamps and deterministic framing.

## Scope

- Delete vector and hybrid types, algorithms, sqlite-vec implementation, build
  option/dependency, dedicated tests and benchmark scenario.
- Delete decay entry points, SQL and dedicated coverage.
- Replace `longterm::Runtime` with `recall(Backend&, RecallRequest)` and bind all
  memory tools to the same backend. Keep search validation in the backend.
- Carry one lexical score in retrieval values. Preserve existing tool-result
  JSON and hook score fields at their rendering/publication boundaries.
- Preserve record metadata, touch semantics, scoped keys, table definitions,
  migration bytes and stored message encoding. Existing optional index tables
  and files are left intact; this slice performs no schema migration or import.

The provider loop, child scheduler, authorization policy and session transaction
contract remain the acceptance boundary. New recall modes and storage cleanup
are separate work.

## Risks And Constraints

- A deletion can remove shared scope/filter/order coverage. Keep lexical and
  end-to-end memory regressions; delete only tests of the removed operations.
- Tool bindings borrow the backend through asynchronous calls. Run isolated
  ASan/UBSan checks on memory and bootstrap, including child cancellation joins.
- Tool output and hooks expose score metadata. Keep their existing lexical
  spelling and null vector field while narrowing internal retrieval values.
- Preserve user databases, including optional extension data. Verify normal
  memory opening does not drop unrelated tables or migration records.
- No new dependencies or heavy public includes. Removing the facade and vector
  implementation should reduce translation units and header declarations.

## Verification

- Run affected memory, bootstrap and tool tests and the retained memory bench.
- Added or behaviorally rewritten tests must fail at their intended assertions
  under isolated faults, then pass with the implementation restored.
- Use an isolated debug copy with explicit ASan/UBSan compiler/linker flags;
  inspect the commands and run the affected lifetime coverage.
- Run `xmake f -y -m release`, `xmake build -j4`, `xmake test -j4` and `make ci`.
- Review all references, package/build inventory and the final diff. Update the
  memory/build contracts and STATUS, delete this completed plan and commit.

## Progress

- [x] Trace production callers, resource owners and dormant retrieval consumers.
- [ ] Reduce memory APIs, composition, dependencies and dedicated coverage.
- [ ] Verify behavior, persistence compatibility and borrowed lifetimes.
- [ ] Update owning documents, remove this plan and commit the complete slice.

## References

- [Memory](../../design-docs/memory-system.md)
- [Runtime composition](../../design-docs/bootstrap-runtime.md)
- [Async ownership](../../design-docs/async-model.md)
- [Live debt](../tech-debt-tracker.md)
- [Testing](../../rules/testing-and-bench.md)
- [Build](../../BUILD_SYSTEM.md)

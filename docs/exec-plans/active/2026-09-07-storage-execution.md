# Audit And Trace Execution

## Objective

Complete the runtime's SQLite execution boundary: audit decisions, audit metadata
and turn traces run on the supplied blocking executor while session coordination
and hooks stay on their strand.

## Current Contracts

- `StorageAuditSink` currently awaits repository operations on its caller.
- The agent loop writes terminal traces on its coordinating executor.
- A pool lease serializes connection access; it does not migrate the coroutine
  that executes SQL.
- Dispatch awaits a durable permission decision before invoking a handler.
- Cancellation joins operations that borrow repositories before their owners
  release them. Cancelled turns retain their terminal trace classification.
- Repository schemas, stored rows and direct repository readback remain intact.

## Complete Slice

1. Bind a blocking executor explicitly to the storage audit adapter and trace
   context, and wire both from runtime composition.
2. Spawn repository writes on that executor and await completion on the caller.
   Keep requests owned across suspension and surface storage/cancellation errors.
3. Verify executor placement, durable permission-before-effect ordering, metadata
   completion and cancelled-turn trace lifetime through real SQLite operations.
4. Remove superseded comments and update the storage, async and tool contracts,
   current status and live debt. Delete this plan after completion.

The change spans permission, agent and bootstrap because those modules own the
effect adapters and their lifetimes. It retains the existing Asio vocabulary and
repository APIs.

## Verification

- Targeted permission, tool, agent and bootstrap builds/tests.
- Separate coordinating and worker executors with explicit synchronization and
  hard timeouts; persisted rows are the completion oracle.
- Isolated deliberate faults must fail the new tests at their intended assertions.
- Explicit ASan/UBSan compiler/linker instrumentation for affected lifetime tests.
- Release configure/build, all test targets and `make ci`.
- Inspect the final diff for unused alternatives, changed storage formats and
  unjoined work before committing the completed slice.

## Completion

- Runtime audit and trace SQL executes on the supplied worker.
- A failed or cancelled pre-effect audit cannot authorize a handler.
- Awaited writes finish before borrowed services can be destroyed, including
  terminal traces after cancellation.
- Existing provider/tool/session, memory and child-session tests pass.

## Progress

- [x] Trace production callers and current ownership contracts.
- [ ] Implement and wire explicit worker execution.
- [ ] Verify executor, ordering and cancellation behavior.
- [ ] Update owning contracts, remove the completed plan and commit.

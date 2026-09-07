# Scheduler Lock Ownership

## Objective

Keep the tool scheduler's path state proportional to live work. A path entry
exists only while a holder, queued waiter or granted waiter needs it. The final
participant releases it synchronously; the host has no maintenance obligation.

## Current Contracts

The library provider/tool/session loop is the acceptance boundary. Parent and
child sessions share a scheduler and coordinating strand. Read locks overlap;
writes exclude other accesses to the same workspace lock key. Cancellation may
return after the batch grace period, but each session joins its own dispatch
context before releasing borrowed services. Spawned calls retain scheduler state
until their guards are released.

The current table retains idle paths and exposes caller-driven TTL reclamation.
No production caller performs that reclamation. Its clocks, configuration and
statistics extend the scheduler surface without owning the resource lifetime.

## Smallest Complete Slice

1. Reclaim an entry when its final holder or waiter exits. Preserve FIFO handoff,
   shared readers and reservations for waiters whose completion is still queued.
   Cancellation reconciles queued and already-granted permits before reclamation.
2. Remove TTL configuration, manual reaping, unused accessors and lock counters.
   Keep the private table's entry count for retention verification. Use guard
   ownership and waiter identity instead of redundant flags or sequence state.
3. Retain the shared scheduler state and context-specific join contract. Replace
   counter-based integration assertions with observable shared-path exclusion.
4. Update the tool and configuration contracts and example; remove stale
   scheduler comments and completed debt. Existing configuration files remain
   readable through the parser's existing treatment of unrecognized nested keys.

This slice adds no tools, host services, background timers or storage migrations.
Persistent user data and the dispatch permission boundary remain intact.

## Verification

- Build and run affected agent, bootstrap and configuration tests.
- Exercise changing paths without manual cleanup, active-entry retention, reader
  sharing, writer exclusion, queued/granted cancellation and subsequent reuse.
- Verify injected scheduler sharing through handler execution; retain child
  cancellation, unrelated-session isolation and late-completion coverage.
- Introduce deliberate faults in an isolated copy and confirm the relevant
  assertions fail, then restore and rerun.
- Run explicit debug ASan/UBSan instrumentation for agent/bootstrap lifetime cases.
- Measure affected translation units before and after using recorded release
  compiler arguments; report local evidence without claiming reference budgets.
- Run the complete release build/test suite and `make ci`; inspect and commit the
  completed slice.

## Completion

No released path remains in the table and no maintenance API or TTL setting
remains. Shared exclusion and context-specific lifetime joins pass their tests.
Durable invariants belong in the owning contracts; remove this plan on completion
and put the next bounded core slice in the handoff.

## Progress

Contract and caller review complete. Implementation has not started.

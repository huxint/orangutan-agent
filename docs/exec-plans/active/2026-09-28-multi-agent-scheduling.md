# Multi-agent scheduling and session admission

## Objective and findings

Preserve independent child sessions, intersected authority, shared path locks and
context-specific cancellation joins while tightening scheduling and admission.
The scheduler eagerly spawns every batch call behind a semaphore; exceptional
exits can consume a permit, and an exception in a dispatch/timeout race can wait
for the timeout. Sessions currently rely on host discipline to avoid overlapping
prompts despite mutable conversation/checkpoint state.

## Complete slice

Launch only max_parallel_tools calls, retain queued calls as values, and launch
the next only after a dispatch has fully completed. Translate dispatch exceptions
inside the timeout race. Validate zero concurrency and nonpositive timeouts at
the Result-returning batch boundary. Cancel only admitted work and distinguish
queued calls from cancellation laggards. Reject overlapping prompts on the same
session until cleanup/persistence finishes. Preserve independent batch permits
so a parent awaiting AgentRun cannot deadlock child tools.

## Verification

Add regressions for queued progress after exceptions, invalid scheduler options,
concurrent session admission/reuse and delegation with one parallel slot. Run
existing cancellation, path-lock, permission and child-limit tests, release suite,
ASan/UBSan for affected ownership, existing scheduler benchmarks and make ci.
Update owning contracts and delete this plan when complete. Process-wide provider
quotas and deeper delegation are separate policies, not changes in this slice.

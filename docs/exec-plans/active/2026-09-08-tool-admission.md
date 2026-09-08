# Tool Admission Boundary

## Objective

Run path exclusion, permission evaluation, approval and execution against the
same finalized tool input. The scheduler owns bounded work; registry dispatch
owns admission through the existing effect boundary.

## Current Contracts

`ToolScheduler` currently parses the provider's original JSON and acquires a path
lock before entering `Registry::dispatch`. The registry can then rewrite the
input through `tool_before`. Permission and filesystem authority use the rewritten
input, while exclusion still uses the original path.

Path entries now exist only for live holders, queued waiters or reserved
handoffs. Shared scheduler state survives late cleanup, and sessions join only
the dispatch contexts they own. These ownership contracts remain in force.

## Smallest Complete Slice

1. Move the existing private path-lock implementation into the tool library.
   Expose a small opaque resource owner that the scheduler retains and dispatch
   borrows. Keep lock-table maintenance and statistics out of the public API.
2. Prepare typed path information once from the final hook input. Use that value
   for the lock request and later pinned authority resolution. The scheduler no
   longer interprets JSON or classifies filesystem capabilities.
3. Acquire the final path lock before resolving filesystem authority, so queued
   reads and writes observe the preceding operation's completed filesystem state.
   Keep permission intersection, exact-input approvals and durable audit before
   handlers. Vetoed calls never acquire path resources.
4. Keep one public dispatch route. Lock waits now occur inside its per-call
   timeout; semaphore admission remains per batch. Cancellation of a queued
   dispatch releases its reservation and finishes observations without effects.
5. Move the private ownership tests to the tool bucket, add behavioral admission
   coverage, update owning contracts and delete replaced scheduler code.

No new tools, host surface, provider behavior or storage schema is introduced.

## Verification

- Calls rewritten from different paths to one path exclude each other; calls
  rewritten from one path to distinct paths can overlap.
- A queued read resolves authority after a preceding writer creates its target.
- Queued lock timeout/cancellation does not execute a handler or strand later
  callers. Shared parent/child/session exclusion and cleanup joins still hold.
- Existing hook, permission, approval, audit failure and memory contracts pass.
- Affected tool/agent/bootstrap builds and tests, the full release suite and
  `make ci` pass. Run explicit ASan/UBSan for changed lifetime boundaries.
- Deliberate isolated faults fail at the intended assertions; restored code
  passes. Compare affected compilation with matching release arguments.

## Completion

The registry uses final input for exclusion and authorization; the scheduler has
no input-parsing responsibility. Document the current contract, record any next
bounded core slice in the handoff, delete this plan and commit the verified work.

## Progress

Contract and call-site review complete. Implementation has not started.

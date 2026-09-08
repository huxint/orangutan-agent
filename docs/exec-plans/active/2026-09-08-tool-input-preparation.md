# Prepared Tool Calls

## Objective

Make filesystem tools prepare one validated request from the final hook input.
Registry dispatch coordinates admission, authorization, audit and execution
without knowing built-in names or parsing their options. Remove the replaced
parsing paths rather than adding another dispatch framework.

## Current Contracts

- The library provider/tool/session loop is the acceptance boundary, including
  scoped recall, persisted continuation and bounded child sessions.
- `tool_before` finalizes input once. Path locks precede pinned authority
  resolution; approval binds exact final bytes and identity. Durable audit
  precedes effects, and completion observations finish under the path lock.
- Ordinary custom handlers retain `Registry::add` and capability-based path
  exclusion. Their names must not select filesystem built-in behavior.
- No persistent schema, stored capability vocabulary or user data changes.

## Smallest Complete Slice

1. Add a synchronous preparation callback returning a per-call execution value
   and optional path intent. Preserve `Registry::dispatch` and adapt ordinary
   handlers through the same pipeline.
2. Move FileRead/FileWrite/FileEdit argument validation into their preparers.
   Execution consumes owned typed arguments; admission uses their path projection.
   Remove built-in name routing and duplicate option/numeric parsing.
3. Reject malformed arguments before path waiting, authority resolution or
   approval. Preserve error/completion observations and audit refusal with the
   final input hash. Keep filesystem-state checks at the effect boundary.
4. Update the owning tool/permission contracts and handoff; delete this plan
   after its invariants and remaining work are recorded there.

No additional tools, application host, memory features or delegation mechanisms
are part of this slice. Those components continue through the existing contracts.

## Verification

- Public dispatch tests cover invalid arguments with ask policies and replay
  grants, hook repair/rewrite, refusal while a path is held, explicit custom
  preparation, and unchanged ordinary-handler behavior.
- Existing path, workspace, approval, audit, scheduler, memory and child-session
  tests retain authority, cancellation and integration coverage.
- Run affected tool/agent/bootstrap tests, the release build and all test targets,
  and `make ci`. Check new regressions with isolated deliberate faults.
- Check prepared-call ownership with explicit ASan/UBSan instrumentation in an
  isolated debug copy. Measure translation-unit cost before and after; report
  local deltas without claiming reference-hardware budget certification.

## Risks And Completion

Prepared callbacks and captured arguments must outlive asynchronous execution;
dispatch retains the prepared value until cleanup finishes. Keep raw input bytes
for approval hashing rather than reserializing parsed arguments. Do not resolve
filesystem state during preparation, because queued calls must observe preceding
effects. Completion requires removal of the old parsing route, passing gates,
updated contracts and a committed implementation.

## Progress

- [x] Read the handoff, architecture and affected contracts; confirm a clean tree.
- [x] Scope preparation as the next core simplification.
- [ ] Implement and remove replaced paths.
- [ ] Verify behavior, ownership and compile cost.
- [ ] Update contracts, remove this plan and commit the completed slice.

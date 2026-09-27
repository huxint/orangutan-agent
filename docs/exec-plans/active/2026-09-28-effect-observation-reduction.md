# Effect Observation Reduction

## Objective

Give each tool, provider and memory effect one gate and one observation.
Collapse hook sinks into owned host callbacks, and make the audit log one
durable decision row per dispatch. Hosts should be able to add a gate or
observer without subclassing, bookkeeping or reading outcomes nobody consumes.

## Current Shape

- `hook::Event` has three duplicates: `tool_error` repeats a failed
  `tool_after`, `tool_dispatched` repeats the decision recorded before the
  effect, and `provider_fallback` repeats `provider_response`'s served target.
- `Sink` is a virtual base with one production implementation,
  `InProcessSink`. The bus borrows raw sink pointers, so an abandoned advisory
  child can outlive a destroyed sink.
- Every runtime caller discards `PublishOutcome`. `unbind`, `binding_count`,
  `sink_count` and `set_options` have no runtime caller.
- `BusOptions::approval_timeout` is never set, so a human approval prompt
  times out after the 2 s extension-hook bound.
- `HookDecision::approval_expires_at` duplicates the rule's approval lifetime.
- Dispatch writes separate `hook_publish` audit rows whose facts already appear
  in the decision row metadata. After execution it rewrites the newest matching
  row to add tool usage (`AuditMetadataUpdate`), matching on the previous
  metadata text. `tool_after` and the turn trace already carry that usage.
- Audit and trace readback APIs are only used by tests. The trace record still
  carries `deferred_catalog_hash`, which the loop always writes as zero.

## Slices

1. **Hooks.** `Sink` becomes an owned value containing an id, a trust flag and
   optional `observe`/`decide` callbacks. `Bus::subscribe` takes ownership,
   and advisory children hold shared ownership until they finish.
   `publish_advisory` returns nothing; sink failures remain isolated. Remove
   the duplicate events and their payloads, the introspection API,
   `PublishOutcome`, `InProcessSink` and `approval_expires_at`. Approval
   prompts wait until they receive a decision or the turn is cancelled.
   Extension gates keep the blocking timeout.
2. **Audit.** Record one durable row per dispatch before any effect; hook
   decisions stay in its metadata. Remove `hook_publish` rows, post-result
   metadata rewrites and their storage operation. Drop the rollup view with a
   new migration; audit rows are untouched. Narrow readback to one scoped
   listing, and remove the retired catalogue hash from the C++ trace values.
   The column remains in existing databases.

Each slice updates [permissions-and-hooks](../../design-docs/permissions-and-hooks.md),
[tool-runtime](../../design-docs/tool-runtime.md) or
[storage-runtime](../../design-docs/storage-runtime.md) in the same commit.

## Boundaries And Risks

- Stored audit, trace and session rows are preserved. Applied migrations are not
  edited; the only new migration drops a derived view.
- Gates still run before effects; redaction still applies to untrusted sinks;
  cancellation still joins or bounds sink children.
- The breaking API changes are intentional under the collaboration policy.
  All in-repository callers, tests and benchmarks migrate directly.

## Verification

- [x] Hooks: `test-hook`, `test-tool`, `test-agent`, `test-bootstrap`, bench build.
- [ ] Audit: `test-storage`, `test-permission`, `test-tool`, a migration on a
  database that already has rows, bench build.
- [ ] Each slice: `xmake build -j4`, `xmake test -j4`, `make ci`.
- [ ] Delete this plan when both slices are complete.

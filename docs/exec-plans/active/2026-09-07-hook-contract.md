# Runtime Hook Contract

## Objective

Expose the hook events produced by the configured provider/tool/memory loop and
one authoritative contract for blocking decisions. Remove reserved lifecycle
vocabulary and payloads without production owners.

## Current Producers

- `agent::Loop` publishes provider request, response, error and fallback metadata.
- `Registry::dispatch` publishes tool before/dispatched/after/error events.
- The memory binding publishes read-after, write-before, write-after and forget.
- Approval resolution publishes `permission_ask_rendered`.

These are 13 of the 41 declared events. Agent/session/channel/team/job reserves,
`memory_read_before` and unused permission observations have no production
publication path. The two job payload types have only synthetic test consumers.
`default_mode` and `hook::Mode` are not used by `Bus`; actual blocking admission
is constrained by `EventTraits` and `HasBlockingDecision`.

## Scope And Constraints

- Reduce `Event` to the current producer vocabulary and remove the two job
  payloads from `Payload`.
- Remove the unused mode enum/table and their dedicated test. Keep blocking
  admission in `EventTraits` for tool-before, memory-write-before and approval.
- Remove job fixtures and their dedicated bus test. Retain ordering, veto,
  redaction, error isolation, timeout and cancellation coverage.
- Keep bus dispatch, task ownership, sink lifetimes, permission decisions and
  bounded child sessions unchanged. This is contract reduction, not new events
  or a change to hook scheduling.
- Preserve database schemas, stored messages, audit JSON and capability names.
  Audit readers retain event metadata as data; they do not parse the retired
  hook names into this enum.
- Update the permissions/hooks contract, current header comments, benchmark
  description and STATUS. Remove completed planning artifacts before delivery.

## Verification

- Verify every retained event against a production publication site and search
  for removed types, modes, fixtures and documentation references.
- Keep compile-time blocking/advisory assertions on current events. Prove a
  changed assertion fails at its intended diagnostic under an isolated trait
  fault, then restore the implementation and run green.
- Run hook, tool and bootstrap tests with explicit ASan/UBSan in an isolated
  debug copy. Inspect actual flags and preserve existing child cleanup tests.
- Build/run the hook benchmark and measure representative hook consumers before
  and after; these local samples are not reference-hardware certification.
- Run `xmake f -y -m release`, `xmake build -j4`, `xmake test -j4`, `make ci`,
  inspect the diff and commit the complete slice.

## Progress

- [x] Trace current producers, decision admission and persistence boundaries.
- [ ] Reduce event/payload vocabulary and remove the unused mode model.
- [ ] Verify dispatch, approval, redaction and lifetime contracts.
- [ ] Update owning documents, remove this plan and commit.

## References

- [Permissions and hooks](../../design-docs/permissions-and-hooks.md)
- [Tool dispatch](../../design-docs/tool-runtime.md)
- [Async ownership](../../design-docs/async-model.md)
- [Live debt](../tech-debt-tracker.md)

# Provider Construction

## Objective

Construct the configured provider through one protocol system that owns endpoint
credentials and borrows an explicit transport. Preserve the library turn boundary,
protocol conversion, retry/fallback policy and HTTP/SSE lifetime guarantees.

## Current Contract

`HttpProviderBackend` is the production consumer of route resolution, adapter
planning, credential bundles and factory registration. Its two registered
factories instantiate the same transport system. An outer profile-routed system
then forwards to those per-profile systems. Adapter names duplicate protocol
enums, and the factory options have no non-default production consumer.

The owning contracts are [providers](../../design-docs/api-portability.md),
[composition](../../design-docs/bootstrap-runtime.md),
[configuration](../../design-docs/secrets-and-state.md) and
[async ownership](../../design-docs/async-model.md).

## Complete Slice

1. Keep pure configuration-to-profile resolution and its owned route projection.
   Remove the unused route-only forwarding API and unconsumed endpoint metadata.
2. Replace adapter plans, public credential wrappers, virtual factories and the
   profile-forwarding system with `make_protocol_system`. It validates every
   selected endpoint and unique profile before reading any credential, then owns
   immutable endpoint credentials in one system.
3. Keep `System` and `ProtocolTransport` as injection boundaries. Dispatch selects
   the profile and verifies model/protocol before transport work. Retry/fallback
   remains in `execution::Runtime`; stream decoders and HTTP limits remain intact.
4. Adapt bootstrap and retain meaningful validation, credential, routing,
   streaming and cancellation tests. Delete fixtures that only test the removed
   factory machinery. Update owning contracts and STATUS, then remove this plan.

No storage schema, stored message format, permission policy, prompt bytes or
external host responsibilities change. Asio remains the async implementation.

## Risks And Verification

- Complete preflight must precede secret lookup, including invalid fallbacks and
  duplicate profiles. Count injected lookups and assert zero on configuration
  errors; retain missing/empty credential and non-secret error checks.
- A selected profile must use its own URL, headers and protocol. Test both
  protocols and real retry/fallback composition; reject mismatched route values
  without transport calls.
- Immutable endpoint storage must survive suspended sends. Exercise concurrent
  calls and joined cancellation with controlled transport synchronization, then
  run explicitly instrumented ASan/UBSan provider/bootstrap ownership cases.
- Run affected release builds/tests, all 14 release test targets, `make ci` and
  Markdown checks. Prove added or behaviorally changed tests against deliberate
  faults in an isolated copy, then restore and rerun them.
- Measure compile commands before/after on this machine. Removed construction
  translation units and headers should reduce the surface; report local timing
  evidence without claiming reference-hardware budget certification.

## Completion

- [x] Trace production consumers and bound the slice.
- [ ] Replace construction and dispatch, including callers and tests.
- [ ] Verify behavioral failures, release integration and instrumented lifetimes.
- [ ] Update contracts and handoff, review the diff, remove the plan and commit.

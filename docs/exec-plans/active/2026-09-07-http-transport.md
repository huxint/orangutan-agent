# Provider HTTP Transport

## Objective

Keep the HTTP/SSE request boundary used by both configured provider protocols.
Remove the unconsumed WebSocket connection stack and make the remaining transport
implementation own its curl resources directly.

## Production Path And Ownership

`HttpProviderBackend` owns `http::Client` and adapts its body and streaming sends
to `provider::ProtocolTransport`. The protocol adapters select HTTP or SSE;
the provider/tool loop and persisted continuation use these existing contracts.
No production component constructs a `WebSocket`.

Each HTTP request owns its curl easy/multi handles and header list. The multi
handle drives bounded polling on the blocking executor so cancellation remains
observable. Its registration guard detaches the easy handle before cleanup.
The async bridge owns the pending request, cancellation flag and executor work;
stream callbacks and completion return through the caller's executor.

## Scope

- Delete the WebSocket public header, facade export, implementation, dedicated
  tests and scripted WebSocket server.
- Move shared curl helpers into `client.cpp`, their sole remaining consumer.
  Use standard unique ownership for the easy/multi handles and keep the global
  curl lifetime, header-list owner and detach guard local to this boundary.
- Preserve body and streaming behavior, cancellation polling, response limits,
  callback ordering and error classification. Keep both public send contracts.
- Keep the stored `egress_websocket` capability spelling; a capability name alone
  does not register or authorize an effect.
- Update provider ownership documentation, public-header comments, the HTTP
  benchmark description and STATUS. No database or message-format changes.

## Risks And Verification

- The HTTP path also needs curl multi handles; retain them and the detach guard.
- Remove only WebSocket-specific tests. Retain HTTP/SSE deadline, byte-limit,
  cancellation, callback-executor and response-parsing coverage.
- Run HTTP and bootstrap tests in an isolated debug copy with explicit
  ASan/UBSan compiler/linker flags and inspect the commands.
- Build and run the existing HTTP benchmark. Measure the affected transport TUs
  before and after on this host without claiming reference-hardware compliance.
- Run `xmake f -y -m release`, `xmake build -j4`, `xmake test -j4` and `make ci`.
- Check all remaining references and archive contents, update the owning
  contracts, delete this completed plan and commit the slice.

## Progress

- [x] Confirm configured-provider callers and request-resource ownership.
- [ ] Remove the unused connection stack and localize HTTP resource owners.
- [ ] Verify the transport and end-to-end runtime contracts.
- [ ] Update documentation, remove this plan and commit.

## References

- [Provider contract](../../design-docs/api-portability.md)
- [Runtime composition](../../design-docs/bootstrap-runtime.md)
- [Async ownership](../../design-docs/async-model.md)
- [Testing](../../rules/testing-and-bench.md)
- [Live debt](../tech-debt-tracker.md)

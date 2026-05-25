# Provider Protocol Transport

Slice 109 adds the provider-owned protocol transport adapter seam needed before
the concrete `oran-http` client lands. New
`<oran/provider/protocol_transport.hpp>` exports `ProtocolTransport`, the
HTTP-shaped `ProtocolHttpRequest` / `ProtocolHttpResponse` value types,
`ProtocolTransportAdapterFactory`, and
`protocol_transport_factory_bindings(anthropic, openai)`. The factory composes
the slice-107 request serializer and slice-108 response decoder, builds
`provider::System` instances for Anthropic Messages or OpenAI Responses
credential targets, injects the provider API-key headers, maps HTTP status
classes into provider error categories, and remains offline-testable through a
fake transport.

The design intent is to prove the concrete protocol-factory shape without
pulling libcurl or a not-yet-shipped `oran-http` target into `oran-provider`.
The current protocol-backed systems force non-streaming JSON body requests;
SSE streaming, real HTTP/TLS I/O, provider hooks, and ordinary binary handoff
remain downstream. Regular `bootstrap::run` still does not read provider
credentials, construct adapters, allocate transport, send requests, or start
`agent::Loop` for ordinary prompts.

Release note: `docs/releases/feature-release-notes.md` documents the injected
transport seam and unchanged bootstrap behavior.

Focused validation:

- `xmake run test-provider` (63 cases / 512 assertions)

Files of interest:

- `include/oran/provider/protocol_transport.hpp`
- `src/oran-provider/protocol_transport.cpp`
- `tests/provider/test_protocol_transport.cpp`
- `include/oran/provider.hpp`
- `docs/exec-plans/active/2026-05-26-provider-adapter-v1.md`
- `docs/design-docs/api-portability.md`
- `docs/product-specs/0001-core-react-loop.md`
- `docs/product-specs/0017-fake-provider-first-agent-loop.md`
- `docs/STATUS.md`

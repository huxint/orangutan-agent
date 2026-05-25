# Provider Protocol Response

Slice 108 adds the provider-owned offline protocol response decoding boundary
needed before HTTP transport. New `<oran/provider/protocol_response.hpp>`
exports `provider::decode_protocol_response(body_json, target)`. The decoder
currently supports Anthropic Messages and OpenAI Responses, maps vendor
text, thinking/reasoning summaries, tool-use blocks, model ids, token usage,
and stop/status fields into the typed `provider::Response` contract, maps
unknown vendor stop/status values to `StopReason::error`, and rejects malformed
JSON or unsupported response item shapes as parsing errors with non-secret
context.

The design intent is to prove response-body mapping before coupling provider
protocols to HTTP status handling, streaming event assembly, retries, or
authentication. Regular `bootstrap::run` still does not read credentials,
construct adapters, allocate transport, send requests, or start `agent::Loop`
for ordinary prompts; transport-backed protocol factories remain under the
active provider-adapter plan.

Release note: `docs/releases/feature-release-notes.md` documents the response
decoder and unchanged bootstrap behavior.

Focused validation:

- `xmake run test-provider` (57 cases / 442 assertions)

Files of interest:

- `include/oran/provider/protocol_response.hpp`
- `src/oran-provider/protocol_response.cpp`
- `tests/provider/test_protocol_response.cpp`
- `include/oran/provider.hpp`
- `docs/exec-plans/active/2026-05-26-provider-adapter-v1.md`
- `docs/design-docs/api-portability.md`
- `docs/product-specs/0001-core-react-loop.md`
- `docs/product-specs/0017-fake-provider-first-agent-loop.md`
- `docs/STATUS.md`

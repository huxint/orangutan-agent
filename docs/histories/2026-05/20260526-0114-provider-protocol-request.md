# Provider Protocol Request

Slice 107 adds the provider-owned offline protocol request serialization boundary
needed before HTTP transport. New `<oran/provider/protocol_request.hpp>` exports
`provider::ProtocolRequest { method, path, body_json }` and
`provider::make_protocol_request(request, target)`. The mapper currently supports
Anthropic Messages and OpenAI Responses, validates opaque tool schema, tool input,
and structured tool-result JSON inside `oran-provider`, preserves text-only
tool-result fallback behavior, and rejects unsupported protocol families as config
errors.

The design intent is to prove vendor JSON request shapes without introducing
network flake, API keys, or binary handoff. Structured tool output also needed a
domain carrier before protocol mapping: `core::ToolResultContent` now preserves
optional `data_json`, and `agent::Loop` copies successful `tool::Output::data_json`
into the provider-facing transcript. Regular `bootstrap::run` still does not read
credentials, construct adapters, allocate transport, send requests, or start
`agent::Loop` for ordinary prompts; response decoding and transport-backed factories
remain under the active provider-adapter plan.

Release note: `docs/releases/feature-release-notes.md` documents the request
mapper and unchanged bootstrap behavior.

Focused validation:

- `xmake run test-core` (71 cases / 455 assertions)
- `xmake run test-provider` (51 cases / 398 assertions)
- `xmake run test-agent` (25 cases / 401 assertions)

Files of interest:

- `include/oran/provider/protocol_request.hpp`
- `src/oran-provider/protocol_request.cpp`
- `tests/provider/test_protocol_request.cpp`
- `include/oran/core/content.hpp`
- `src/oran-agent/loop.cpp`
- `tests/agent/test_loop.cpp`
- `docs/exec-plans/active/2026-05-26-provider-adapter-v1.md`
- `docs/design-docs/api-portability.md`
- `docs/product-specs/0014-structured-tool-output.md`
- `docs/STATUS.md`

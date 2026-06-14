# Provider Adapter v1 Handoff

## Goal

Build the first real provider-adapter path in small, reviewable slices: typed
config route metadata becomes an adapter-ready plan, credentials are resolved at
an explicit secret boundary, protocol families serialize domain requests into
vendor JSON bytes, transport-backed factories construct concrete systems, and
ordinary binary prompts switch from the deterministic no-runner shell to
`cli::run_async` with `bootstrap::AgentPromptRunner`.

## Scope

- In scope:
  - Preserve the fake-provider-first loop contract while real adapters land.
  - Keep provider request/response protocol mapping inside `oran-provider`.
  - Keep public headers heavy-include-free by exposing serialized bytes and stdlib
    values, not JSON parser types.
  - Construct one backend per resolved route profile and leave retry/fallback in
    `provider::execution::Runtime`.
  - Switch ordinary `bootstrap::run` to `cli::run_async` only after a real backend
    can be constructed from config without test-only shims.
- Out of scope:
  - Provider usage/cost aggregation beyond the current `provider::Usage` values.
  - Real-provider CI that requires live API keys.
  - New route policy fields such as cost, context window, or capabilities.
  - Web/channel streaming UI work.

## Context

- Relevant docs:
  - `docs/design-docs/api-portability.md`
  - `docs/design-docs/bootstrap-runtime.md`
  - `docs/product-specs/0001-core-react-loop.md`
  - `docs/product-specs/0014-structured-tool-output.md`
  - `docs/product-specs/0017-fake-provider-first-agent-loop.md`
  - `docs/product-specs/0018-first-loop-observability.md`
- Relevant code paths:
  - `include/oran/provider/*`
  - `src/oran-provider/*`
  - `src/oran-bootstrap/bootstrap.cpp`
  - `src/oran-agent/loop.cpp`
  - `tests/provider/*`, `tests/bootstrap/*`, `tests/agent/*`
- Constraints:
  - `bootstrap::run` must not read API-key environment variables or send network
    traffic until the concrete adapter path exists.
  - Provider protocol code may use `nlohmann_json` privately in `.cpp` files; public
    headers must keep the compile-budget boundary.
  - Structured tool output must preserve the text fallback and let the agent loop,
    not protocol code, decide which structured bytes are sent.
- Compile-budget impact:
  - Slice 107 adds private JSON serialization to `oran-provider`; the dependency is
    documented in `docs/BUILD_SYSTEM.md` and `docs/rules/libraries.md`.
    Slice 108 reuses that private dependency for offline response decoding.
    Slice 109 adds only provider value types plus an abstract injected
    transport; it does not add a third-party dependency.
    Slice 110 adds the first concrete platform HTTP target, links system
    `libcurl >=8.11.0` privately behind `oran-http::Client`, and keeps curl
    handles out of public headers. Slice 111 adds the bootstrap-owned
    `HttpProviderBackend` construction seam over that client; it introduces no
    new third-party dependency and keeps libcurl ownership in `oran-http`.

## Risks

- Risk: vendor JSON details leak into `oran-agent` while adapters are being built.
  Mitigation: keep all request serialization and future response decoding inside
  `oran-provider`; agent tests assert only domain `provider::Request` values.
- Risk: ordinary bootstrap starts reading secrets or sending requests before the
  prompt handoff switch is intentional.
  Mitigation: keep `HttpProviderBackend::build` as an explicit construction
  boundary, keep regular `bootstrap::run` on the no-runner path until the next
  slice, and test both backend construction and the existing bootstrap path.
- Risk: response decoding and streaming are coupled too early.
  Mitigation: land offline request serialization first, then offline response
  decoding, then a non-streaming injected transport seam before SSE streaming.
- Risk: structured tool output silently falls back to text.
  Mitigation: add provider protocol tests that prove `ToolResultContent::data_json`
  reaches each protocol's supported tool-result shape and an agent-loop test
  that proves `tool::Output::data_json` reaches the provider-facing transcript.

## Milestones

1. Resolve route profiles and build an offline adapter construction plan.
2. Resolve adapter credentials at an explicit environment-secret boundary.
3. Build a profile-routed adapter system from caller-registered factories.
4. Serialize provider-domain requests into Anthropic Messages and OpenAI Responses
   JSON bytes offline.
5. Decode Anthropic/OpenAI responses back into `provider::Response`.
6. Add protocol-backed factories over an injected transport, then wire that seam
   through a concrete HTTP client.
7. Switch ordinary binary prompts to `cli::run_async` with `AgentPromptRunner` when
   backend construction is available.

## Validation

- Commands:
  - `xmake run test-provider`
  - `xmake run test-agent`
  - `xmake run test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `make ci`
  - `git diff --check`
- Manual checks:
  - Confirm `bootstrap::run` still avoids `resolve_adapter_credentials` and
    `make_adapter_system` until transport-backed factories exist.
  - Confirm provider public headers do not include `nlohmann/json.hpp`.
- Observability checks:
  - Keep trace/audit behavior unchanged until ordinary binary handoff starts using
    `AgentPromptRunner`.
- Bench comparison:
  - Not required for offline JSON serialization slices unless a hot-path tradeoff is
    introduced.

## Progress Log

- [x] Slice 103: preserve route profile endpoint metadata for adapter construction.
- [x] Slice 104: build the offline adapter construction plan and bootstrap preflight.
- [x] Slice 105: resolve adapter credentials from configured environment variables.
- [x] Slice 106: dispatch resolved credentials through registered adapter factories.
- [x] Slice 107: serialize Anthropic Messages / OpenAI Responses request JSON bytes
      offline and preserve structured tool-result data through the agent loop
      before protocol-specific request mapping.
- [x] Slice 108: decode Anthropic Messages / OpenAI Responses response JSON bytes
      offline into `provider::Response`.
- [x] Slice 109: build Anthropic/OpenAI protocol factories over an injected
      body-response `ProtocolTransport`.
- [x] Slice 110: add the concrete `oran-http` / libcurl body-response client target.
- [x] Slice 111: adapt `http::Client` to `provider::ProtocolTransport` and
      construct provider backends from bootstrap config.
- [x] Slice 112: switch configured-route ordinary binary prompts to
      `cli::run_async` with `AgentPromptRunner` and `HttpProviderBackend`.
- [x] Move this plan to `docs/exec-plans/completed/` once the binary handoff lands.

## Decision Log

- 2026-05-26: Keep request serialization as an offline `ProtocolRequest` boundary
  before adding HTTP transport. This lets the test suite prove vendor JSON shapes and
  structured tool-result mapping without API keys or network flake.
- 2026-05-26: Keep response decoding as an offline `decode_protocol_response`
  boundary before transport. It proves model/content/usage/stop-reason mapping
  without coupling to HTTP status handling or streaming assembly.
- 2026-05-26: Put the first protocol factories behind an injected
  `ProtocolTransport` instead of waiting for libcurl. This proves factory/header/
  status/request-response composition while keeping real HTTP and SSE streaming
  in the platform transport slice.
- 2026-05-26: Keep retry/fallback outside protocol factories. The adapter-factory
  seam constructs single-target backends; `provider::execution::Runtime` remains the
  owner of retry and fallback policy.
- 2026-05-26: Land `oran-http` as a platform body-response client before bootstrap
  adapter construction. The client takes a caller-owned blocking executor so libcurl
  work can run on `async::Runtime::cpu_executor()` rather than the main coroutine
  executor, while future SSE support can extend the same library boundary.
- 2026-05-26: Keep the first concrete backend as an explicit
  `bootstrap::HttpProviderBackend` object instead of immediately switching
  `bootstrap::run`. That lets tests prove credentials, HTTP transport, protocol
  factories, and route ownership without changing ordinary prompt behavior until the
  final `cli::run_async` handoff slice.
- 2026-05-26: Switch configured-route `bootstrap::run` to
  `HttpProviderBackend` plus `AgentPromptRunner`, while preserving the built-in
  no-route defaults on the deterministic no-runner shell. This makes the
  credential and network boundary explicit for configured operators without
  making fresh checkouts require provider credentials.

## Linked Artifacts

- Related design doc: `docs/design-docs/api-portability.md`
- Related product specs:
  - `docs/product-specs/0001-core-react-loop.md`
  - `docs/product-specs/0014-structured-tool-output.md`
  - `docs/product-specs/0017-fake-provider-first-agent-loop.md`
  - `docs/product-specs/0018-first-loop-observability.md`
- History entries:
  - `docs/histories/2026-05/20260526-0008-provider-adapter-factory.md`
  - `docs/histories/2026-05/20260526-0114-provider-protocol-request.md`
  - `docs/histories/2026-05/20260526-0228-provider-protocol-response.md`
  - `docs/histories/2026-05/20260526-0302-provider-protocol-transport.md`
  - `docs/histories/2026-05/20260526-0426-http-body-client.md`
  - `docs/histories/2026-05/20260526-0627-bootstrap-provider-backend.md`
  - `docs/histories/2026-05/20260526-0738-bootstrap-provider-handoff.md`
- Release note:
  - `docs/releases/feature-release-notes.md`

# API Portability

This document describes the provider abstraction: how the runtime sends a domain
`provider::Request` to *some* LLM, *some* protocol, *some* transport, and how the
result comes back as a `provider::Response`. The legacy `orangutan/` design (transport
→ protocol → execution) was sound; v2 keeps the shape but slims the surface and removes
the `stdexec` dependency.

## Domain Model

```cpp
namespace orangutan::provider {

struct Request {
  std::vector<core::Message> messages;
  std::optional<std::string> system_prompt;
  std::vector<core::ToolDef> tools;
  std::optional<std::string> tool_choice;     // "auto", "any", "<name>"
  std::optional<std::uint32_t> max_tokens;
  std::optional<std::uint32_t> thinking_budget;
  bool                       stream = true;
  std::optional<PromptCacheHints> cache;
  RetryPolicy                retry;             // execution-layer concern
};

struct Response {
  std::vector<core::Content> blocks;            // text, tool_use, thinking, ...
  StopReason                 stop_reason;
  Usage                      usage;             // input, output, cache tokens
  std::optional<std::string> model_used;        // model id for fallback observability
};

}  // namespace orangutan::provider
```

`core::Content` is a typed variant; protocol adapters translate to/from vendor JSON.

> **Status (slice 112, 2026-05-26):** `oran-provider` exists as the
> provider-domain, prompt-cache-hint, fake-provider, route-resolver, first
> execution wrapper, offline protocol request/response serialization library,
> and injected body-response protocol transport seam.
> `oran-http` now also exists as the platform HTTP/TLS target: `<oran/http.hpp>`
> exports `http::Header`, `http::BodyRequest`, `http::BodyResponse`, and
> `http::Client`, a pimpl-backed libcurl body client whose constructor receives
> the executor used for blocking curl work.
> `<oran/provider.hpp>` exports the slice-73 value shapes (`Request`,
> `Response`, `Usage`, `RetryPolicy`, `PromptCacheHints`,
> `PromptCacheOptions`, `make_prompt_cache_hints(prompt::RenderedPrompt,
> options)`) plus the slice-74 system surface (`ProtocolKind`,
> `ModelTarget`, `Route`, abstract `provider::EventSink` with default
> no-op delta callbacks, abstract `provider::System::send(Request, Route,
> EventSink*) const`, and the first concrete `provider::FakeProvider`
> with `ScriptedTurn` / `StreamDelta`) plus slice-97
> `provider::execution::Runtime`, a `provider::System` decorator that
> consumes `Request::retry`, retries retryable errors per target, tries
> `Route::fallbacks` after primary exhaustion, observes cancellation during
> backoff, suppresses retry/fallback after visible stream output, and fills
> missing `Response::model_used` from the selected target, plus slice-98
> `provider::resolve_route(Config, route_name)`, which resolves configured
> profile/route names into a `provider::Route`, preserves fallback order, maps
> provider spellings and exact `ProtocolKind` names, and since slice 102
> prefers an explicit `profiles.<name>.protocol` field over the vendor label
> so self-hosted gateways can reuse a shipped protocol without pretending to be
> a built-in provider. Slice 103 adds
> `provider::resolve_route_profiles(Config, route_name)`, returning
> `provider::RouteProfileResolution` with the same loop-facing targets plus
> endpoint metadata (`provider`, `base_url`, `api_key_env`) for the future
> adapter factory; `api_key_env` is still only the configured environment
> variable name. Slice 104 adds `provider::make_adapter_construction_plan`,
> which validates non-empty endpoint metadata and `http://` / `https://`
> base-url schemes, records the protocol adapter-family name selected for each
> primary/fallback profile, and derives the same loop-facing `provider::Route`.
> Slice 105 adds `provider::resolve_adapter_credentials(plan)`, which is the
> first explicit provider secret-read API: it reads the API-key environment
> variables named by the adapter plan, stores only in-memory key strings beside
> the matching plan targets, derives the same loop-facing route, and reports
> missing or empty keys as `ErrorKind::auth` with non-secret context. The
> slice-106 `provider::make_adapter_system(credentials, factories)` consumes
> that credential bundle plus caller-registered `ProtocolAdapterFactory`
> bindings, constructs one backend per primary/fallback profile, and returns a
> profile-routed `provider::System` that forwards each single-target execution
> call to the matching backend. The factory validates missing/null/duplicate
> bindings and duplicate route profiles as config errors without logging
> secret API-key values. Slice 107 adds `<oran/provider/protocol_request.hpp>`
> with `provider::ProtocolRequest { method, path, body_json }` and
> `provider::make_protocol_request(request, target)`, supporting
> `ProtocolKind::anthropic_messages` and `ProtocolKind::openai_responses`.
> That offline mapper converts the typed `provider::Request` /
> `core::Message` / `core::Content` contract into vendor JSON body bytes,
> validates opaque tool schema, tool input, and structured tool-result JSON in
> the provider implementation TU, preserves text-only tool-result fallbacks,
> and maps `ToolResultContent::data_json` into the structured Anthropic/OpenAI
> tool-result channels. Slice 108 adds
> `<oran/provider/protocol_response.hpp>` with
> `provider::decode_protocol_response(body_json, target)`, supporting the same
> Anthropic Messages and OpenAI Responses protocol families. That offline
> decoder maps vendor text, thinking/reasoning summaries, tool-use blocks,
> model ids, token usage, and terminal status/stop reasons back into
> `provider::Response` while rejecting malformed JSON or unsupported response
> item types as `ErrorKind::parsing`. Slice 109 adds
> `<oran/provider/protocol_transport.hpp>` with the HTTP-shaped
> `ProtocolHttpRequest` / `ProtocolHttpResponse` value types, abstract
> `ProtocolTransport`, `ProtocolTransportAdapterFactory`, and
> `protocol_transport_factory_bindings(anthropic, openai)`. That factory builds
> Anthropic Messages or OpenAI Responses `provider::System` backends from
> resolved credential targets, composes the request serializer with the response
> decoder, injects provider API-key headers, maps HTTP status classes into
> provider error categories, rejects route profile/model/protocol mismatches
> before sending, and forces `request.stream=false` while the injected transport
> returns one JSON body. The
> resolvers and plan report `Error::config` for missing profile references,
> unknown provider spellings, unknown explicit protocol spellings, or malformed
> adapter endpoint metadata. Slice 111 adds bootstrap's
> `HttpProviderBackend`, which adapts `http::Client` to
> `provider::ProtocolTransport`, resolves configured credentials, registers
> the built-in Anthropic/OpenAI protocol factories, and owns the transport plus
> produced `provider::System` for `AgentPromptRunner` callers. Slice 112
> switches configured-route `bootstrap::run` to construct that backend and call
> `cli::run_async` with `AgentPromptRunner`, so ordinary binary `--prompt` runs
> now drive `agent::Loop` through the HTTP-backed Anthropic/OpenAI body-response
> systems. Built-in empty defaults still use the deterministic no-runner CLI
> shell and read no provider credentials. Slice 101's
> bootstrap `AgentPromptRunner` is the loop owner that consumes a resolved
> route plus `provider::execution::Runtime` to drive `agent::Loop` with a
> caller-supplied backend. Slice 121 adds the platform SSE transport in
> `oran-http` (`http::SseEvent`, `Client::send_streaming(BodyRequest,
> SseEventCallback)`, an incremental `text/event-stream` parser, and the
> blocking-executor→coroutine-executor event handoff so the decoder/sink never
> run on the curl thread). Slice 122 adds the provider-side Anthropic streaming:
> a stateful `AnthropicSseDecoder` (the incremental sibling of
> `decode_protocol_response`), `ProtocolTransport::send_streaming(ProtocolHttpRequest,
> ProtocolSseCallback)` plus a `ProtocolTransport::supports_streaming()`
> capability gate, and the Anthropic `ProtocolTransportSystem` now honoring
> `request.stream` — it drives `send_streaming` + the decoder when the caller
> asks to stream and the transport advertises support, otherwise it keeps the
> single-body path (OpenAI Responses stays body-only this arc). Retry/fallback
> stream-suppression needs no new code: the slice-97 `execution::Runtime`
> already wraps the caller's `EventSink` in a per-attempt `AttemptSink`, so a
> pre-first-byte failure retries while a post-first-byte failure returns
> `stream_already_emitted`. Slice 123 wires the path into the binary: bootstrap's
> `HttpProtocolTransport` overrides `supports_streaming()` to `true` and
> implements `send_streaming` over `http::Client::send_streaming`, and
> `cli::StreamingPromptSink` renders the provider's `EventSink` deltas live to the
> terminal while `AgentPromptRunner` wires it into non-quiet streaming runs — so
> ordinary configured-route `orangutan --prompt` over Anthropic now streams tokens
> character-by-character (spec 0001 AC3). Provider hooks and usage/cost rollups
> remain planned.

## Layered Implementation

```
┌──────────────────────────────────────────────────────────────┐
│ oran-provider::System (public entry)                          │
│   Request → Response                                          │
└────────────────────────┬─────────────────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────────┐
│ execution::Runtime                                            │
│   retry, fallback model; usage/cost/hooks later              │
└────────────────────────┬─────────────────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────────┐
│ protocol::Adapter (one per API shape)                         │
│   anthropic_messages, openai_chat_completions,                │
│   openai_responses, gemini, custom_openai_compatible          │
└────────────────────────┬─────────────────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────────┐
│ transport (asio-based http + SSE)                             │
│   oran-http::Client (HTTP, SSE, streaming bodies)             │
└──────────────────────────────────────────────────────────────┘
```

### Public Entry

```cpp
class System {
 public:
  virtual async::Awaitable<core::Result<Response>>
  send(Request, Route, EventSink* sink = nullptr) const = 0;
};

namespace execution {
class Runtime final : public System {
 public:
  explicit Runtime(System& backend);
  async::Awaitable<core::Result<Response>>
  send(Request, Route, EventSink* sink = nullptr) const override;
};
}  // namespace execution
```

`Route` is the resolved primary + fallbacks. `EventSink` is the streaming hook: the
adapter calls `sink->on_text_delta`, `sink->on_thinking_delta`,
`sink->on_tool_start`, `sink->on_tool_delta`, `sink->on_done` as the stream progresses.

### Routes

```cpp
struct Route {
  ModelTarget                 primary;
  std::vector<ModelTarget>    fallbacks;
};

struct ModelTarget {
  std::string profile;              // config-defined profile key
  std::string model;                // vendor model id
  ProtocolKind protocol;            // enum: anthropic_messages, openai_chat, …
  std::optional<std::uint32_t> thinking_budget;
  std::optional<PromptCacheOptions> cache;
  Capabilities caps;                // streaming, tool_use, thinking, vision, …
};
```

Profiles live in config; routes can be defined globally or per-agent. Slice 98
adds the first resolver, `provider::resolve_route(config, route_name)`, for the
current top-level config shape. It looks up `config.routes()[route_name]`, maps
the primary and fallback profile names through `config.profiles()`, preserves
the fallback order authored by the operator, and fills `ModelTarget` with the
profile key, vendor model id, and resolved `ProtocolKind`. Slice 102 adds the
optional `profiles.<name>.protocol` field: when present, the resolver parses it
as an exact `ProtocolKind` spelling and uses it ahead of provider-name aliases;
when absent, existing provider aliases such as `anthropic`, `openai`, and
`deepseek` still infer the protocol. Slice 103 adds
`provider::resolve_route_profiles(config, route_name)` for adapter factories:
it returns `RouteProfileResolution { primary, fallbacks }` where each
`ResolvedProfileTarget` carries the resolved `ModelTarget` plus the profile's
`provider`, `base_url`, and `api_key_env`. `RouteProfileResolution::route()`
derives the existing `Route` value for loop/execution callers, so this richer
surface does not change `agent::Loop`. Slice 104 adds
`provider::make_adapter_construction_plan(resolution)`, which validates those
resolved endpoint fields for the future factory, classifies each target by the
selected protocol adapter family, and still does not read API-key environment
variables or allocate a transport. Slice 105 adds
`provider::resolve_adapter_credentials(plan)` as the next explicit factory
input: it resolves the plan's `api_key_env` names through `std::getenv`,
returns `ErrorKind::auth` for missing or empty environment variables, keeps
error context to non-secret identifiers (`role`, `profile`, `api_key_env`),
and yields `AdapterCredentialBundle { primary, fallbacks }` that can derive
the same loop-facing `Route`. Slice 106 adds
`provider::make_adapter_system(credentials, factories)`: it matches each
credential target's adapter-family name to a registered
`ProtocolAdapterFactory`, constructs a backend for each route profile, and
returns a `provider::System` that routes single-target execution calls by
`route.primary.profile`. Slice 107 adds the offline
`provider::make_protocol_request(request, target)` seam that protocol factories
call before HTTP transport; slice 108 adds the paired
`provider::decode_protocol_response(body_json, target)` seam. Slice 109 adds
`ProtocolTransportAdapterFactory`, which composes those mappers over an injected
`ProtocolTransport` to build non-streaming Anthropic/OpenAI body-response
systems without pulling libcurl into `oran-provider`. Slice 110 adds the
platform `oran-http::Client` body transport, and slice 111 adds bootstrap's
`HttpProviderBackend`, which adapts that client to `ProtocolTransport`,
resolves credentials, registers the built-in transport factories, and returns a
profile-routed `provider::System` plus route for `AgentPromptRunner` callers.
Slice 112 consumes that backend in configured-route `bootstrap::run` before
`cli::run_async`.
They currently
serialize/decode Anthropic Messages and OpenAI Responses bodies, including
text/thinking/tool-use blocks, usage counters, model ids, stop reasons, and
text-only/structured tool-result request mapping, and reject unsupported
protocol families as configuration errors. The current typed config does not yet
carry route-level `thinking_budget`, prompt-cache options, or capability
metadata, so those fields stay unset until their schema lands. The agent's
`Loop` will resolve a `Route` once per turn (or once per `Loop` if static) and
reuse it across iterations.

### Protocol Adapters

```cpp
class Adapter {
 public:
  virtual ~Adapter() = default;

  virtual Capabilities capabilities() const = 0;

  virtual async::Awaitable<core::Result<Response>>
  send(http::Client&, const Profile&, const ModelTarget&, const Request&,
       EventSink* sink) = 0;
};
```

Built-in adapters:

| ProtocolKind                   | Vendor format                                  | Notes |
| ------------------------------ | ---------------------------------------------- | ----- |
| `anthropic_messages`           | Anthropic Messages API (`/v1/messages`)        | Streaming, extended thinking, tool_use |
| `openai_chat_completions`      | OpenAI Chat Completions (`/v1/chat/completions`) | Streaming, function calling |
| `openai_responses`             | OpenAI Responses API (`/v1/responses`)         | Streaming, multimodal |
| `gemini_generate_content`      | Google Gemini (`/v1beta/models/.../generateContent`) | Streaming via SSE |
| `deepseek_chat`                | OpenAI-compatible                              | Just an alias entry in profiles |
| `custom_openai_compatible`     | Generic OpenAI-compatible                      | For self-hosted / 3rd-party providers |

Adding a new protocol = adding a new `Adapter` subclass and an entry in
`ProviderRegistry`. The legacy code already had this shape; v2 inherits.

### Capabilities

A `Capabilities` bitfield per route lets the agent code branch on what's available
without testing provider names:

```cpp
struct Capabilities {
  bool streaming           = false;
  bool tool_use            = false;
  bool extended_thinking   = false;
  bool vision_image        = false;
  bool audio_in            = false;
  bool prompt_cache        = false;
  bool json_mode           = false;
  bool parallel_tool_calls = false;
  std::uint32_t max_context_tokens = 0;
};
```

Code asks `route.primary.caps.tool_use`, not `route.is_anthropic()`. Vendor lock-in is
avoided by construction.

### Execution Layer

Concerns owned by `execution::Runtime`:

- **Retry**: configurable per-route. Idempotent retry for transport errors, no retry
  for upstream-classified semantic errors. **Status (slice 97):** shipped as a
  `provider::System` decorator over any backend `System`; each target gets its
  own `Request::retry.max_attempts` budget, `initial_backoff` is awaited
  through cancel-aware `async::sleep_for`, and `retry_after` can extend that
  delay. If an attempt has already emitted visible `EventSink` output, the
  retry is skipped so callers do not render duplicate stream bytes.
- **Fallback**: on retryable failure of the primary, try fallbacks in order.
  **Status (slice 97):** shipped for `Route::fallbacks`; each backend call
  receives a single-target route, and fallback successes fill missing
  `Response::model_used` with the selected target model. Fallback is also
  skipped after visible stream output for the same duplicate-output reason.
- **Usage aggregation**: per-agent, per-route, per-day counters in `audit.db`.
  Planned.
- **Cost tracking**: profiles declare cost/1M tokens; aggregator computes spend and
  emits `provider.cost_threshold` hooks. Planned.
- **Hooks**: `provider_request` / `provider_response` / `provider_error` /
  `provider_fallback` (see `permissions-and-hooks.md`). Planned.

## Streaming

All adapters support streaming when the vendor supports it. The contract:

- The `Awaitable<Result<Response>>` resolves only when the stream terminates.
- During streaming, the `EventSink` (provided by the caller) receives deltas.
- The web UI's SSE route bridges these deltas to the client.
- The CLI REPL renders deltas to the terminal in real time.

**Status (slices 121–123):** the Anthropic Messages streaming path is shipped
end-to-end — from the platform transport (`oran-http`, slice 121) through the
provider decoder/System (slice 122) to the binary + terminal (slice 123).

- **Transport seam.** `ProtocolTransport` carries a body `send` plus a streaming
  `send_streaming(ProtocolHttpRequest, ProtocolSseCallback)` where
  `ProtocolSseCallback = void(std::string_view event, std::string_view data)` is
  **provider-owned** so `oran-http` types stay off the provider's public surface
  (symmetric with `ProtocolHttpRequest/Response`). A `supports_streaming()`
  capability query (default `false`) lets a transport that only does body
  requests — or has not yet implemented streaming — stay on the body path; an
  adapter streams only when both the request asks for it and the transport
  advertises support. On a 2xx event stream `send_streaming` resolves with
  status + headers and an **empty** body (events already delivered); any other
  response returns the body for the caller to decode through the same
  HTTP-status→category path the body `send` uses.
- **Decoder.** `AnthropicSseDecoder` (in `src/oran-provider/_impl/`, `nlohmann`
  private to its `.cpp`) is the stateful sibling of `decode_protocol_response`.
  It runs a strict state machine — `message_start`→usage/model;
  `content_block_start`→open text/thinking/tool_use (+`on_tool_start`);
  `content_block_delta`→append + `on_text_delta`/`on_thinking_delta`/`on_tool_delta`;
  `content_block_stop`→finalize (parse accumulated tool-input JSON);
  `message_delta`→stop_reason + output usage; `message_stop`→assemble `Response`
  + `on_done`; `ping` ignored; an `error` event or malformed payload records the
  first decode error. The assembled `Response` is byte-for-byte the one the body
  decoder produces for the equivalent non-streaming body.
- **System.** The Anthropic `ProtocolTransportSystem::send` honors
  `request.stream`: when set (and the transport supports streaming) it sends
  `stream=true`, drives `send_streaming` + the decoder, and returns the assembled
  `Response`; otherwise it forces `stream=false` and takes the body path. OpenAI
  Responses stays body-only until its own SSE decoder follow-up.
- **Retry/fallback suppression — already sufficient (slice 97).** The streaming
  System simply calls the `EventSink*` it is handed, which `execution::Runtime`
  has already wrapped in a per-attempt `AttemptSink`. A pre-first-byte failure
  emitted nothing, so it retries/falls back normally; once any `on_*` delta has
  fired, a later retryable error returns immediately with
  `retry_skipped` / `fallback_skipped = stream_already_emitted` so terminal/UI
  callers never see duplicate stream bytes. No streaming-specific suppression
  code exists — the slice-97 machinery covers it.

**Slice 123** wires the path end-to-end into the binary. Bootstrap's
`HttpProtocolTransport` overrides `supports_streaming()` to `true` and implements
`send_streaming` over `http::Client::send_streaming`, translating each
`http::SseEvent` into the provider `ProtocolSseCallback`. `cli::StreamingPromptSink`
(a `provider::EventSink`) renders answer and thinking deltas to the terminal as
they arrive — flushing per delta for character-by-character output — plus a
one-line `[tool: <name>]` marker per tool call. `AgentPromptRunner` constructs the
sink for non-quiet streaming runs, passes it to `agent::Loop::run_turn`, and clears
the assembled `PromptRunResult::text` once the answer streamed live so the CLI does
not print it twice. Ordinary configured-route `orangutan --prompt` over Anthropic
now renders tokens character-by-character (spec 0001 AC3); a mid-stream Ctrl-C
surfaces as `Error::cancelled` with `cancellation_phase=provider` (spec 0018,
reused). The OpenAI Responses SSE decoder remains the noted post-arc follow-up.

## Caching

Prompt caching is **first-class**. `prompt::Builder` produces a
`RenderedPrompt` with ordered `CacheSection`s, per-section content hashes,
`prefix_hash`, and `prefix_bytes`. Adapters map these to vendor cache APIs:

- Anthropic: `cache_control: { type: "ephemeral" }` blocks.
- OpenAI Responses: prompt prefix hashing where available.
- Others: silently ignored.

The cache discipline that governs which content may appear in which section,
and the byte-identical-preamble invariant the prompt builder must honor, live
in [`docs/rules/prompt-design.md`](../rules/prompt-design.md). That rule is
the canonical home for what goes where; this doc owns the adapter-side
mapping only.

Slice 73 adds the first shared mapping helper:
`provider::make_prompt_cache_hints(rendered, options)` validates that a
rendered prompt has exactly the seven prompt-design sections, exactly one
breakpoint, and that the breakpoint is the final prefix section before the
conversation tail. It then maps sections 1-6 to adapter-facing
`PromptCacheSectionKey { id, content_hash, cache_version }` entries and
copies the prefix hash/byte count. The conversation tail is deliberately
excluded. `PromptCacheOptions` lets a route disable prompt caching or skip it
when the prefix is below a provider-specific minimum byte floor; real
Anthropic/OpenAI wire mapping still belongs to the adapter slices.

### Cache Key Versioning

Each section carries a `cache_version` integer. When upstream caching is
provider-managed (Anthropic), the key is opaque; when we need to invalidate (e.g., a
skill activated/deactivated), we bump the version. The legacy code's string-concat
cache key is replaced by a tuple `(section_id, content_hash, version)`.
`prompt::Builder` includes each section version in `prefix_hash`, so a version
bump invalidates the cached prefix even when content bytes are unchanged.

## Configuration Shape

```jsonc
{
  "profiles": {
    "anthropic-main": {
      "provider": "anthropic",
      "protocol": "anthropic_messages",
      "model": "claude-3-5-sonnet-latest",
      "base_url": "https://api.anthropic.com",
      "api_key_env": "ANTHROPIC_API_KEY"
    },
    "openai-main": {
      "provider": "openai",
      "model": "gpt-5.5",
      "base_url": "https://api.openai.com/v1",
      "api_key_env": "OPENAI_API_KEY"
    }
  },
  "routes": {
    "default":  { "primary": "anthropic-main", "fallbacks": ["openai-main"] },
    "coder":    { "primary": "anthropic-main" },
    "research": { "primary": "openai-main" }
  }
}
```

This is the current `oran-config` foundation shape and matches `config.example.json`:
each profile key names one provider/model tuple, and routes reference those profile
keys. The optional `protocol` field is for profiles whose vendor/operator label
should stay distinct from the wire protocol, for example a self-hosted gateway using
the OpenAI Responses shape. If it is omitted, `provider::resolve_route` falls back to
the provider alias table. `provider::resolve_route_profiles` preserves the profile's
endpoint metadata for the future adapter factory without reading `api_key_env`.
`provider::make_adapter_construction_plan` preflights that metadata without
constructing adapters or sending network traffic, and
`provider::resolve_adapter_credentials` is the later, explicit secret-read step
that resolves the named API-key environment variables for concrete factories.
`provider::make_adapter_system` is the following construction seam: it consumes
the credential bundle plus registered protocol factories and returns a
profile-routed `provider::System`. Slice 109's
`ProtocolTransportAdapterFactory` can now supply Anthropic Messages and OpenAI
Responses backends over an injected body-response `ProtocolTransport`; slice
110 adds `oran-http::Client` for real HTTP/TLS body I/O, and slice 111's
`bootstrap::HttpProviderBackend` binds that client into `ProtocolTransport` for
explicit backend construction. Slice 112 uses that backend for configured-route
ordinary binary prompts. SSE streaming and provider hooks still land in later
slices. Slices 107-108 add offline request-body
serialization and response-body decoding for Anthropic Messages and OpenAI
Responses before that transport step.
Custom headers, context windows, thinking policy, and cost fields remain planned
provider-schema fields until the typed parser accepts them.

## Error Categories

```cpp
enum class ErrorCategory {
  config,          // misconfiguration — not retryable
  auth,            // bad credentials — not retryable
  network,         // transport error — retryable
  rate_limit,      // upstream throttle — retryable with backoff
  upstream,        // vendor 5xx — retryable
  parsing,         // we got something we didn't expect — log + escalate
  invalid_request, // we sent something the vendor rejected — not retryable
  interrupted,     // cancelled
  unknown,
};
```

`Error::retryable()` is the canonical predicate; execution layer uses it.

## Anti-Patterns

- Reaching into protocol-specific JSON in agent code. Always go via `Response::blocks`.
- Per-adapter retry logic. Retry is execution-layer.
- Mixing async styles. Adapters return `Awaitable<Result<Response>>`, period.
- Hardcoding model names. Use config profiles and routes.

## Bench

`bench/provider/` ships:

- `bench/provider/scenarios/cache_mapping.cpp` —
  `provider.cache_hints_enabled` validates and maps a `RenderedPrompt` prefix
  to adapter-facing cache keys, while `provider.cache_hints_disabled` is the
  route-level off-switch baseline.

Planned adapter benches:

- `request_encode` — serializing a 32-message conversation to vendor JSON.
- `response_decode` — parsing a streamed response (synthetic; no network).
- `protocol_overhead` — A/B between adapters on the same canonical request.

## See Also

- [`async-model.md`](async-model.md) — how streams interact with the executor.
- [`../product-specs/0001-core-react-loop.md`](../product-specs/0001-core-react-loop.md)
  — first concrete v1 deliverable.
- Legacy `src/providers/` for reference (not copy).

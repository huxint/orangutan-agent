# Provider Boundary

`provider::System::send` consumes typed messages, tool definitions, model policy
and a route, and returns a typed response. The agent loop does not branch on a
vendor name. HTTP transport is injected through the protocol adapter boundary.

## Construction

`resolve_route_profiles` converts configured names, aliases and model policy into
owned endpoint values. `make_protocol_system` validates the complete route before
reading any credential: endpoint fields are nonempty, URLs use HTTP/S, protocols
are implemented and profile names are unique. An invalid fallback therefore
fails before primary credential lookup. Secret lookup failures expose only
profile, role and credential-reference context.

The returned `System` owns immutable model/endpoint credentials for every profile
and borrows one `ProtocolTransport`. Each send selects a profile and checks its
model and protocol before transport work. The caller supplies a single selected
target; `execution::Runtime` owns retry and fallback selection. Concurrent sends
keep their request and stream-decoder state separate. The transport, system and
event sink remain alive until their awaited sends, callbacks and cancellation
cleanup complete.

## Protocols

Implemented protocols are `anthropic_messages` and `openai_responses`. A profile
selects protocol, model, endpoint and credential reference. `HttpProviderBackend`
owns the HTTP transport and constructed provider system; callers may inject a
controlled provider for tests.

Adapters translate roles, content, tool calls/results, stop reasons and usage.
They validate malformed responses and expose provider errors explicitly. Retry
and fallback belong to `provider::execution::Runtime`, not individual protocol
mappers. Attempt metadata records the model/route actually used.

## Streaming And Bounds

HTTP/SSE transport enforces request deadlines and response byte caps. SSE decoders
maintain protocol state, assemble text/tool input incrementally and reject
incomplete or inconsistent terminal events. Cancellation propagates through the
blocking transport bridge to curl. Stream callbacks are observations of the
current turn; a partial stream is not a completed persisted answer.

`http::Client` owns HTTP/SSE request execution. Each request holds unique curl
easy/multi handles and a header list on the blocking executor. The registration
guard detaches the easy handle before either handle is released. Bounded multi
polling observes cancellation. The pending operation retains its implementation,
request values, cancellation flag and executor work until completion; SSE events
run serially on the caller's executor and finish before the send returns.

Fallback applies the selected profile's thinking and cache policy. A provider
error must retain its category and attempt attribution. Credential lookup errors
contain non-secret context only.

## Prompt Caching

`oran-prompt` renders stable text and fingerprints the same native declarations
sent in `Request::tools`. Descriptions and input schemas occur only in those
native declarations. The effective prefix identity includes their bytes and
cache version, while conversation messages remain dynamic.

`Request::cache` carries only a caller-owned `prefix_hash` and `prefix_bytes` for
`system_prompt` and native tools. The loop supplies these values without applying
primary policy, and retry/fallback execution preserves them for every attempt.
Provider has no dependency on prompt rendering or its section layout. Callers
must keep the identity aligned with the stable fields they submit; conversation,
including system messages supplied through `Request::messages`, is separate.

`make_protocol_request` applies the selected target's `cache` policy, including
direct sends and every retry/fallback. Absent policy defaults to enabled with a
zero-byte floor. Missing hints, a zero-byte prefix, a disabled target, a prefix
below its byte floor, or no stable system text/native tools omit explicit cache
controls. Equality with the byte floor is eligible. A primary's disabled policy
or higher floor cannot suppress controls for an eligible fallback.

| Protocol | Explicit control |
| --- | --- |
| `anthropic_messages` | Stable system text becomes a text block ending in `cache_control: {"type":"ephemeral"}`. With no stable system text, the last native tool receives the breakpoint. System messages lifted from conversation follow that breakpoint, and conversation/tool results receive no markers. |
| `openai_responses` | `prompt_cache_key` is `oran-` followed by the prefix hash as 16 lowercase hexadecimal digits. Conversation changes preserve this routing key; changed prefix identity changes it. |

These fields follow the [Anthropic prompt caching](https://platform.claude.com/docs/en/build-with-claude/prompt-caching)
and [OpenAI prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching)
protocols. Disabled policy omits client cache controls; it does not disable
OpenAI's automatic caching. No retention override is sent. Byte eligibility is
local policy, not a provider token threshold. Controlled transport tests establish
request encoding and route behavior, not service cache hits or retention.

The section-mapping function `make_prompt_cache_hints` and
`PromptCacheSectionKey` are removed. Embedders construct `PromptCacheHints` from
their stable-prefix hash and byte count; they no longer supply section keys or
breakpoint indices. The standard loop already supplies this value.

Changing model, route or stable inputs changes the effective request. Dynamic
timestamps, turn IDs, trace IDs and tool results do not enter the system preamble.
[prompt-design](../rules/prompt-design.md) owns section placement and fingerprints.

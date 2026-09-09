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

`make_prompt_cache_hints` validates the final stable-text breakpoint and counts
native tool fields toward the configured byte floor. Retry/fallback execution
applies the selected profile's eligibility policy. Current protocol encoders do
not serialize explicit cache directives from these internal hints; protocol
cache-control support remains in [live debt](../exec-plans/tech-debt-tracker.md).
Local fingerprint tests do not establish provider cache hits.

Changing model, route or stable inputs changes the effective request. Dynamic
timestamps, turn IDs, trace IDs and tool results do not enter the system preamble.
[prompt-design](../rules/prompt-design.md) owns section placement and fingerprints.

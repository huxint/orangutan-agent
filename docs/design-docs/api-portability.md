# Provider Boundary

`provider::System::send` consumes typed messages, tool definitions, model policy
and a route, and returns a typed response. The agent loop does not branch on a
vendor name. HTTP transport is injected through the protocol adapter boundary.

## Protocols

Implemented protocols are `anthropic_messages` and `openai_responses`. A profile
selects protocol, model, endpoint and credential reference. Route validation
precedes credential lookup. `HttpProviderBackend` owns the resulting factories,
HTTP transport and provider system; callers may inject a controlled provider
for tests.

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

Fallback applies the selected profile's thinking and cache policy. A provider
error must retain its category and attempt attribution. Credential lookup errors
contain non-secret context only.

## Prompt Caching

`oran-prompt` renders ordered sections and hashes their content with explicit
cache versions. The adapter maps that representation into protocol-specific
cache hints. Tool descriptions and memory framing are deterministic for equal
inputs; conversation messages remain after the cached prefix.

Changing model, route or stable section bytes changes the effective request.
Dynamic timestamps, turn IDs, trace IDs and tool results do not enter the stable
system preamble. [prompt-design](../rules/prompt-design.md) owns section placement.

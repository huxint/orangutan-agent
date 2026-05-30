# Provider SSE Streaming

## Goal

Land real Server-Sent-Events streaming end-to-end so a configured-route
`orangutan --prompt` over Anthropic Messages renders tokens to the terminal
character-by-character. Platform `oran-http::Client` grows a `send_streaming`
entry over libcurl's existing streaming body; `oran-provider` adds a stateful
`AnthropicSseDecoder` plus `ProtocolTransport::send_streaming`, and the Anthropic
`ProtocolTransportSystem` stops forcing `stream=false`; bootstrap's
`HttpProviderBackend` adapts the http streaming surface onto a provider-owned
callback; and `cli::StreamingPromptSink` renders `provider::EventSink` deltas
live. The arc closes spec 0001 AC3 ("REPL renders streaming tokens
character-by-character") and spec 0017 v1.1's "Streaming sink" item. The agent
loop is **unchanged** — it already accepts an `EventSink*`
([`loop.hpp:177`](../../../include/oran/agent/loop.hpp)) and threads it into the
provider call ([`loop.cpp:493`](../../../src/oran-agent/loop.cpp)); streaming is
invisible above the provider boundary, and a streamed `tool_use` is just a
`Response` block assembled from `input_json_delta` chunks. OpenAI Responses SSE
is a noted post-arc follow-up.

## Scope

- In scope:
  - **oran-http (platform — the generic wire standard).**
    - New public `http::SseEvent { std::string event; std::string data; }`
      (`event` defaults to `"message"`; `data` is the multi-line-joined SSE
      `data:` payload).
    - New `Client::send_streaming(BodyRequest, SseEventCallback)
      → Awaitable<Result<BodyResponse>>` where
      `SseEventCallback = std::function<void(const SseEvent&)>`. On a 2xx
      `text/event-stream` response it dispatches events and resolves with
      status + headers and an **empty** body; on a non-stream response (errors)
      it accumulates the body and returns it for the caller to decode. Reuses
      the existing `BodyResponse` type.
    - Internal incremental parser `src/oran-http/_impl/sse_parser.hpp` (feed
      byte chunks → emit complete events; `\n` / `\r\n`, `field: value`,
      multi-line `data:`, blank-line dispatch, ignore `:`-comment / `id:` /
      `retry:`). Parses inside the libcurl write callback on the blocking
      executor; each complete event is `post`-ed to the send coroutine's
      executor **before** the callback fires, so the decoder and sink never run
      on the curl thread.
    - Mid-stream cancellation through the existing curl multi poll loop.
    - **Add an `oran-http` compile-budget category** (see corrections below).
  - **oran-provider (composition — vendor semantics).**
    - `ProtocolTransport::send_streaming(ProtocolHttpRequest,
      ProtocolSseCallback) → Awaitable<Result<ProtocolHttpResponse>>`, with the
      provider-owned callback type `void(std::string_view event,
      std::string_view data)` so `oran-http` stays off the provider's public
      surface (symmetric with how `ProtocolHttpRequest/Response` — not
      `http::BodyRequest` — already form the transport seam).
    - `AnthropicSseDecoder` — stateful incremental sibling of
      `decode_protocol_response`
      ([`protocol_response.hpp:23`](../../../include/oran/provider/protocol_response.hpp)):
      `message_start`→usage/model; `content_block_start`→open text/thinking/
      tool_use (+`sink->on_tool_start`); `content_block_delta`→text/thinking/
      input_json deltas (append + matching `sink->on_*`);
      `content_block_stop`→finalize (parse accumulated tool-input JSON);
      `message_delta`→stop_reason + output usage; `message_stop`→assemble
      `Response` + `sink->on_done`; `ping` ignored; `error`→`ErrorKind::parsing`
      / `upstream`. `nlohmann` stays in the `.cpp`.
    - The Anthropic `ProtocolTransportSystem::send` honors `request.stream`:
      when set, it drives `send_streaming` + the decoder; otherwise the existing
      body path. The `request.stream = false` force at
      [`protocol_transport.cpp:138`](../../../src/oran-provider/protocol_transport.cpp)
      is removed on the Anthropic streaming path only. OpenAI stays body-only.
  - **oran-bootstrap.** `HttpProtocolTransport::send_streaming`
    ([`provider_backend.cpp:27`](../../../src/oran-bootstrap/provider_backend.cpp))
    implemented by calling `http::Client::send_streaming` and translating
    `http::SseEvent` → the provider callback (it already implements body
    `send`).
  - **oran-cli / AgentPromptRunner.** A `cli::StreamingPromptSink :
    provider::EventSink` that writes `on_text_delta` live to stdout (flushed),
    a one-line `[tool: <name>]` on `on_tool_start`, and minimal
    `on_thinking_delta`. `AgentPromptRunner` constructs it when not quiet, sets
    `RunTurnInputs::stream=true`, and passes it to `run_turn(inputs, &sink)`
    (today the runner calls `run_turn` with **no** sink at
    [`prompt_runner.cpp:250`](../../../src/oran-bootstrap/prompt_runner.cpp)).
    The runner still returns the assembled `PromptRunResult{text}`.
- Out of scope:
  - **OpenAI Responses SSE decoder** — post-arc follow-up (Slice 124), mirrors
    Slice B. OpenAI's factory stays body-only this arc.
  - Web UI SSE bridge (spec 0007); gemini / other-protocol streaming.
  - Any change to `agent::Loop` control flow — it already accepts and threads
    the `EventSink*`. The loop acts on the assembled `Response` exactly as
    today.
  - Any change to the five `provider::EventSink` callbacks — frozen by slice 74
    / spec 0017 ([`system.hpp:96-120`](../../../include/oran/provider/system.hpp)).
  - **New retry/fallback suppression machinery** — already shipped (slice 97;
    see Decision Log).
  - New third-party dependency — libcurl already streams.

## Context

- Relevant docs (and where each slice syncs them):
  - [`design-docs/api-portability.md`](../../design-docs/api-portability.md)
    "Streaming" (currently a stub, ~lines 321-329) + "Execution Layer"
    (slice-97 stream-suppression note) — **Slice B** promotes "Streaming" to
    the full streaming contract and records the AttemptSink-already-suffices
    note.
  - [`ARCHITECTURE.md`](../../ARCHITECTURE.md) — the `oran-http` row currently
    ends "streaming/SSE plus server/router surfaces remain planned"; the
    `oran-bootstrap` narrative references `oran-http::Client`. **Slice A** flips
    the http row; **Slice C** updates the bootstrap narrative.
  - [`product-specs/0001-core-react-loop.md`](../../product-specs/0001-core-react-loop.md)
    AC3 (REPL renders streaming tokens char-by-char) + the libcurl mid-stream
    cancel risk — **Slice C** marks AC3 shipped.
  - [`product-specs/0017-fake-provider-first-agent-loop.md`](../../product-specs/0017-fake-provider-first-agent-loop.md)
    v1.1 "Streaming sink" (lines 259-261) + per-slice status notes — **Slice
    C** marks the item shipped.
  - [`product-specs/0018-first-loop-observability.md`](../../product-specs/0018-first-loop-observability.md)
    `cancellation_phase=provider` — reused for mid-stream cancel, reference
    only.
  - [`rules/async-and-concurrency.md`](../../rules/async-and-concurrency.md) —
    no blocking on the agent executor; cross-executor wake must post onto the
    waiter's own executor (the F3/F23 oran-io singleflight class of bug).
  - [`design-docs/async-model.md`](../../design-docs/async-model.md) — how
    streams interact with the executor.
  - [`rules/compile-budget.md`](../../rules/compile-budget.md) — **Slice A**
    adds the `oran-http` row here and in `compile_budget.json` (same commit).
  - **Not** [`design-docs/io-runtime.md`](../../design-docs/io-runtime.md) — that
    is the `oran-io` (local filesystem) doc, not `oran-http` (see corrections).
- Relevant code paths (verified 2026-05-30):
  - oran-http: [`include/oran/http/client.hpp`](../../../include/oran/http/client.hpp)
    (`Client` :47, `send` :63, `BodyRequest` :29 / `BodyResponse` :39 /
    `Header` :22); [`src/oran-http/client.cpp`](../../../src/oran-http/client.cpp)
    (`write_body` :205-210, `write_header` :212-232, `CURLOPT_WRITEFUNCTION`
    :266; `run_multi` poll loop :294-323 with `kPollTimeoutMs=50` :32 and the
    cancel check :308-309; `send_blocking` ~:357; awaitable `send` ~:433-447).
    New parser: `src/oran-http/_impl/sse_parser.hpp`.
  - oran-provider:
    [`include/oran/provider/protocol_transport.hpp`](../../../include/oran/provider/protocol_transport.hpp)
    (`ProtocolTransport` :52, `send` :62; `ProtocolHttpRequest` :31 /
    `ProtocolHttpResponse` :40);
    [`src/oran-provider/protocol_transport.cpp`](../../../src/oran-provider/protocol_transport.cpp)
    (`ProtocolTransportSystem` :125, `send` :132, `request.stream=false` :138);
    [`include/oran/provider/protocol_response.hpp`](../../../include/oran/provider/protocol_response.hpp)
    :23 (`decode_protocol_response` — decoder sibling);
    [`include/oran/provider/system.hpp`](../../../include/oran/provider/system.hpp)
    (`EventSink` :85, callbacks :96/:101/:107/:113/:120; `System` :133, `send`
    :151);
    [`src/oran-provider/execution.cpp`](../../../src/oran-provider/execution.cpp)
    (`AttemptSink` :20-56; wrap+check :114-135 — **unchanged**, reused).
  - oran-bootstrap:
    [`src/oran-bootstrap/provider_backend.cpp`](../../../src/oran-bootstrap/provider_backend.cpp)
    (`HttpProtocolTransport` :27, body `send` :32-61, `Impl` :72);
    [`include/oran/bootstrap/provider_backend.hpp`](../../../include/oran/bootstrap/provider_backend.hpp)
    :31.
  - oran-agent / oran-cli:
    [`include/oran/agent/loop.hpp`](../../../include/oran/agent/loop.hpp)
    (`RunTurnInputs::stream` :101; `run_turn(..., EventSink* = nullptr)`
    :177-178); [`src/oran-agent/loop.cpp`](../../../src/oran-agent/loop.cpp)
    (sink → `provider_.send` :493);
    [`include/oran/bootstrap/prompt_runner.hpp`](../../../include/oran/bootstrap/prompt_runner.hpp)
    (`AgentPromptRunnerOptions::stream` :49 / `quiet` :52; `run_prompt` :82);
    [`src/oran-bootstrap/prompt_runner.cpp`](../../../src/oran-bootstrap/prompt_runner.cpp)
    (`inputs.stream` :234; `run_turn` with no sink :250);
    [`include/oran/cli/operator_prompt_sink.hpp`](../../../include/oran/cli/operator_prompt_sink.hpp)
    :38 (`OperatorPromptSink` — a `hook::Sink`; `StreamingPromptSink` is
    net-new, no `provider::EventSink` exists in `oran-cli` yet).
- Constraints:
  - The parser parses bytes on the blocking/curl thread, but every complete
    event is posted to the send coroutine's executor before the callback fires;
    the decoder + `EventSink` run on the agent strand in production, never on
    the curl thread (async-and-concurrency A-rules; mirrors the F3/F23
    cross-executor wake fix).
  - `oran-http` public headers stay libcurl-free (curl handles private to
    `client.cpp`); `SseEvent` / `SseEventCallback` are stdlib-shaped.
  - `oran-provider` public surface stays `oran-http`-free → the provider owns
    `ProtocolSseCallback`.
  - No `nlohmann` in `oran-provider` public headers — `AnthropicSseDecoder`
    keeps `nlohmann` in the `.cpp`.
  - No new third-party dependency.
- Compile-budget impact:
  - `oran-http` has **no** category in `compile_budget.json` today (slice 110
    omitted it). Slice A adds one. Proposed starting triple: mirror `oran-async`
    `{ median 1.0, p95 2.0, hard_cap 2.5 }` (oran-http links libcurl + asio like
    async); **finalize from a measured `client.cpp` TU** via
    `scripts/measure-tu.sh`. Update `compile_budget.json` +
    `docs/rules/compile-budget.md` in the same commit.
  - `oran-provider` `{ median 1.5, p95 3.0, hard_cap 3.5 }` unchanged. The
    decoder is new `.cpp` weight; if `protocol_response.cpp` nears the cap,
    split into `src/oran-provider/_impl/anthropic_sse_decoder.*` (decide in
    Slice B from a measurement). No public-header weight added.

## Risks

- Risk: **Decoder/sink running on the curl thread.** Mitigation: the parser
  parses bytes on the blocking executor, but every complete event is
  `asio::post`-ed to the send coroutine's executor before the provider
  callback (and thus the decoder + `EventSink`) runs. Pin with a test that
  asserts the callback executor identity.
- Risk: **Cross-executor wake races** (the F3/F23 oran-io singleflight class).
  Mitigation: post onto the waiter's own executor; capture by value; the curl
  thread hands off only the parsed bytes and touches no shared mutable state.
- Risk: **Duplicate caller-rendered output on retry/fallback.** RESOLVED, not a
  live risk — slice-97 `AttemptSink` returns `stream_already_emitted` once any
  `on_*` fired (pre-first-byte failures still retry). Slice B pins both arms
  through `execution::Runtime` + a fake streaming transport.
- Risk: **Partial stdout on mid-stream cancel.** Accepted behavior (spec 0001
  AC4 + spec 0018 `cancellation_phase=provider`). Test asserts `Error::cancelled`
  mid-stream; already-printed bytes are expected.
- Risk: **SSE grammar edge cases** — CRLF vs LF, multi-line `data:`, comment /
  `id:` / `retry:` lines, `event` default `"message"`, blank-line dispatch,
  chunk boundaries that split a field mid-event. Mitigation: parser unit tests
  with adversarial chunk boundaries.
- Risk: **Anthropic event-ordering assumptions.** Mitigation: the decoder is a
  strict state machine fed a canned `message_start → … → message_stop`
  sequence; the test asserts the assembled `Response` equals the body-path
  `decode_protocol_response` output for the same logical content.
- Risk: **Localhost SSE test fixture.** Mitigation: reuse the slice-110/111
  localhost mock-server pattern (and the proxy-robustness hardening already
  applied to http/bootstrap fixtures). No network / no keys in CI.
- Risk: **New compile-budget category gate fires.** Mitigation: keep
  `sse_parser.hpp` header-only and template-free; measure `client.cpp` after
  Slice A and set the triple from the measurement.

## Milestones

1. **Slice 121 (A) — oran-http SSE streaming.** `SseEvent`, `sse_parser.hpp`,
   `Client::send_streaming`, executor marshaling, non-2xx error-body fallback,
   mid-stream cancel; add the `oran-http` compile-budget category. Self-contained
   platform slice — ships and tests on its own with a localhost mock server.
2. **Slice 122 (B) — provider Anthropic SSE.** `ProtocolTransport::send_streaming`
   + `ProtocolSseCallback`, `AnthropicSseDecoder`, Anthropic
   `ProtocolTransportSystem` honoring `request.stream` (remove the `:138`
   force on the streaming path), and `execution::Runtime` suppression
   integration (verify only — no new code). Drives end-to-end through a fake
   streaming `ProtocolTransport`.
3. **Slice 123 (C) — bootstrap + cli wiring.** `HttpProtocolTransport::send_streaming`;
   `cli::StreamingPromptSink`; `AgentPromptRunner` constructs the sink when not
   quiet and passes `&sink` to `run_turn`. Lights up spec 0001 AC3.
4. **Follow-up (Slice 124, post-arc) — OpenAI Responses SSE decoder.** Mirrors
   Slice B; separate history; not gating this arc.

## Validation

- Commands:
  - `xmake build oran-http && xmake build test-http && xmake run test-http`
    (Slice A).
  - `xmake build oran-provider && xmake run test-provider` (Slice B).
  - `xmake build oran-bootstrap oran-cli && xmake run test-bootstrap &&
    xmake run test-cli` (Slice C).
  - `scripts/check-compile-budget.sh` after Slice A adds the `oran-http`
    category — gate the new category.
  - `make ci` — docs + STATUS freshness gate, each slice.
  - **Operator-only live smoke (not CI):**
    `xmake run orangutan -- --config config.example.json --prompt "…"` with a
    real `ANTHROPIC_API_KEY` — observe char-by-char streaming and a Ctrl-C
    mid-stream cancel < 1 s (spec 0001 AC3/AC4).
- Manual checks: per slice, `docs/STATUS.md` bumped (slice, last-history, test
  counts) with a matching history under `docs/histories/2026-05/`.
- Observability checks: a mid-stream cancel writes a `trace_turns` row with
  `stop_reason=cancelled`, `cancellation_phase=provider` (spec 0018, reused);
  assert in the Slice C round-trip test.
- Bench comparison (only if uncertainty surfaces): a synthetic `response_decode`
  bench comparing `AnthropicSseDecoder` against body-path
  `decode_protocol_response` on the same logical content (api-portability
  "Planned adapter benches"). Defer unless Slice B raises a perf question —
  bench to resolve uncertainty, not for blanket coverage.

## Progress Log

- [x] Slice 121 (A): confirm SSE grammar coverage list; implement `SseEvent` +
      `sse_parser.hpp` + `Client::send_streaming` + executor marshaling +
      error-body fallback + mid-stream cancel.
- [x] Slice 121 (A): add the `oran-http` compile-budget category to
      `compile_budget.json` + `docs/rules/compile-budget.md` (same commit).
- [x] Slice 121 (A): tests — parser units (chunk-split, CRLF, multi-line
      `data:`, comments), localhost `text/event-stream` integration, mid-stream
      cancel, callback-executor identity.
- [x] Slice 121 (A): **docs in the same PR** — `ARCHITECTURE.md` oran-http row,
      `api-portability.md` transport bullet; `STATUS.md` (slice, `test-http`
      counts); history (`docs/rules/docs-in-sync.md`).
- [x] Slice 122 (B): `ProtocolTransport::send_streaming` + `ProtocolSseCallback`;
      `AnthropicSseDecoder`; Anthropic System honors `request.stream` (remove
      the `protocol_transport.cpp:138` force on the streaming path). Added a
      `ProtocolTransport::supports_streaming()` capability gate (default false)
      so the streaming path activates only for a stream-capable transport — see
      the Decision Log.
- [x] Slice 122 (B): verify `execution::Runtime` `AttemptSink` suppression
      covers streaming (no new code); test both arms (pre-first-byte retry;
      post-first-byte `stream_already_emitted`).
- [x] Slice 122 (B): tests — canned Anthropic sequence → ordered `EventSink`
      callbacks + assembled `Response` (text, `tool_use` from
      `input_json_delta`, usage, stop_reason); fake streaming `ProtocolTransport`
      drives the streaming System end-to-end (mirrors the existing
      fake-transport pattern). Decoder equality with `decode_protocol_response`
      pinned; `test-provider` 66 / 528 → 81 / 614.
- [x] Slice 122 (B): docs — `api-portability.md` "Streaming" full contract +
      AttemptSink note; spec 0017 status; `STATUS.md`; `QUALITY_SCORE.md`
      provider row; history.
- [x] Slice 123 (C): `HttpProtocolTransport::send_streaming` (over
      `http::Client::send_streaming`, translating `http::SseEvent`) +
      `supports_streaming()` → `true`; `cli::StreamingPromptSink`;
      `AgentPromptRunner` wiring (construct the sink when not quiet + streaming,
      pass `&sink` to `run_turn`, clear the duplicate text once it streamed).
- [x] Slice 123 (C): tests — localhost SSE round trip through
      `HttpProviderBackend` (capturing `EventSink`, asserts `"stream":true` +
      decoded text); `StreamingPromptSink` rendering via an injected
      `std::ostringstream`; runner streams to an injected sink + suppresses the
      duplicate text; binary E2E `--prompt` now answers SSE. `test-cli` 14 / 97 →
      18 / 110; `test-bootstrap` 72 / 316 → 75 / 344.
- [x] Slice 123 (C): docs — `ARCHITECTURE.md` cli/bootstrap rows; spec 0001 AC3
      shipped; spec 0017 v1.1 "Streaming sink" shipped; `api-portability.md`
      "Streaming" shipped end-to-end; `feature-release-notes.md`; `STATUS.md`;
      `QUALITY_SCORE.md` cli/bootstrap/provider rows; history.
- [x] Arc close: plan moved to `completed/`; the OpenAI Responses SSE decoder
      (Slice 124) remains the noted post-arc follow-up (not gating).

## Decision Log

- 2026-05-30 (Slice C): **Lift the streaming gate by overriding
  `supports_streaming()` in `HttpProtocolTransport` only.** Slice B left the
  capability query at its `false` default so every transport stayed body-only
  between slices. Slice C overrides it to `true` (plus implements
  `send_streaming`) in the single transport that can stream, so configured-route
  Anthropic now streams by default — which flipped the two binary-level tests
  that asserted `"stream":false` (`test_provider_backend.cpp`,
  `test_bootstrap.cpp`). Those were updated truthfully: the provider-backend test
  keeps a body-path case pinned to `stream=false` and adds a real SSE round-trip
  case; the binary E2E test now answers `text/event-stream` and asserts
  `"stream":true`. OpenAI Responses stays body-only (its transport is the same
  `HttpProtocolTransport`, but the System forces `stream=false` for OpenAI).
- 2026-05-30 (Slice C): **Injectable `std::ostream`, and suppress the duplicate
  final-text print only when text actually streamed.** `StreamingPromptSink` and
  `AgentPromptRunnerOptions::stream_out` take an `std::ostream*` (default
  `std::cout`) so the sink is unit-testable with an `std::ostringstream`. The
  runner clears `PromptRunResult::text` after the turn *only when*
  `StreamingPromptSink::rendered_answer_text()` holds — a non-streaming/body-path
  turn fires no text deltas, so its assembled text is still returned for the CLI
  to print. Gating on the sink's own observation (not on `stream_`) keeps both
  the streaming and non-streaming paths correct.

- 2026-05-30 (Slice B): **Added a `ProtocolTransport::supports_streaming()`
  capability gate (default `false`), not in the original plan.** `ProtocolTransport`
  is shared by the test fakes and bootstrap's `HttpProtocolTransport`, whose
  `send_streaming` override is Slice C. Honoring `request.stream` (default `true`)
  unconditionally would route configured-route production — and the
  `test-bootstrap` localhost round trip that asserts the wire body contains
  `"stream":false` — through the not-yet-implemented streaming path the moment
  Slice B removes the `:138` force. Gating the Anthropic streaming branch on
  `request.stream && protocol==anthropic_messages && transport.supports_streaming()`
  keeps every slice green: the fake streaming transport overrides the query to
  `true` and exercises the decoder end-to-end, while `HttpProtocolTransport` keeps
  the default and stays body-only until Slice C overrides both `supports_streaming()`
  and `send_streaming`. Chosen over a sentinel-error fallback (no double-send, no
  error-code coupling). `send_streaming` itself is a non-pure (defaulted) virtual
  for the same slice-safety reason — a pure virtual would make `HttpProtocolTransport`
  / the test `RecordingTransport` abstract and break the `oran-bootstrap` build
  between B and C.
- 2026-05-30 (Slice B): **Decoder is its own `_impl` TU, not appended to
  `protocol_response.cpp`.** This is the plan's compile-budget contingency taken
  proactively: the new `_impl/anthropic_sse_decoder.cpp` measures on par with
  `protocol_response.cpp` (≈ 2.6 s reference-equivalent), so no `nlohmann` provider
  TU grows toward the 3.5 s cap and `compile_budget.json` is unchanged.
- 2026-05-30: **One exec-plan covering slices 121-123 (A/B/C).** Multi-library,
  multi-slice arc with new public API in more than one library — matches
  `PLANS_GUIDE.md` "create a plan" triggers and the tool-scheduler-v1 precedent
  (one plan, slices 116-120). OpenAI Responses decoder is a noted follow-up, not
  a gating slice.
- 2026-05-30: **Open item RESOLVED — no new retry/fallback suppression
  machinery.** The slice-97 `execution::Runtime` already wraps the caller's
  `EventSink` in `AttemptSink` (`execution.cpp:20`) and checks `emitted()` after
  a retryable error (`:131`). The streaming Anthropic System simply calls the
  `EventSink*` it is handed (already `AttemptSink`-wrapped by the Runtime), so
  pre-first-byte failures retry/fallback and post-first-byte failures return
  `retry_skipped` / `fallback_skipped=stream_already_emitted`. Slice B only adds
  tests; it writes no suppression code. This answers the source plan's lone
  "open item to confirm."
- 2026-05-30: **Provider-owned `ProtocolSseCallback`
  `void(std::string_view event, std::string_view data)`.** Keeps `oran-http`
  types off `oran-provider`'s public surface — symmetric with how
  `ProtocolHttpRequest/Response` (not `http::BodyRequest`) already form the
  transport seam.
- 2026-05-30: **`send_streaming` returns status+headers + empty body on a 2xx
  event stream; accumulates the body on a non-stream (error) response.** Errors
  are decoded by the caller via the existing HTTP-status→category + body-error
  path. Reuses `BodyResponse`; no new error machinery.
- 2026-05-30: **Parse on the curl thread, dispatch off it.** The parser parses
  bytes inside the libcurl write callback (blocking executor) but posts each
  complete event to the send coroutine's executor before the callback fires —
  the decoder and `EventSink` never run on the curl thread
  (async-and-concurrency A-rules; mirrors the F3/F23 cross-executor wake fix).
- 2026-05-30: **CORRECTION to the source plan — oran-http surface docs are
  `ARCHITECTURE.md` (oran-http row) + `api-portability.md` (transport /
  Streaming), NOT `io-runtime.md`.** `io-runtime.md` documents `oran-io` (local
  filesystem) and is not touched by this arc.
- 2026-05-30: **CORRECTION to the source plan — this arc DOES change
  `compile_budget.json`.** `oran-http` has no category today (slice 110 omitted
  it); Slice A adds one (+ `compile-budget.md`), contrary to the source plan's
  "no category change." The triple is set from a measured `client.cpp` TU.
- 2026-05-30: **Anthropic streaming only this arc.** The OpenAI Responses System
  stays body-only (keeps forcing `stream=false`) until the follow-up decoder, to
  avoid a half-migrated OpenAI streaming path.

## Linked Artifacts

- Related design doc:
  [`design-docs/api-portability.md`](../../design-docs/api-portability.md)
  ("Streaming", "Execution Layer"); [`ARCHITECTURE.md`](../../ARCHITECTURE.md)
  (oran-http row).
- Related product spec:
  [`product-specs/0001-core-react-loop.md`](../../product-specs/0001-core-react-loop.md)
  (AC3);
  [`product-specs/0017-fake-provider-first-agent-loop.md`](../../product-specs/0017-fake-provider-first-agent-loop.md)
  (v1.1 "Streaming sink").
- PRs: TBD (one per slice).
- History entry: one per slice under `docs/histories/2026-05/`.
- Release note: Slice 123 adds a `docs/releases/feature-release-notes.md` entry
  (user-visible: the REPL / CLI streams tokens live).

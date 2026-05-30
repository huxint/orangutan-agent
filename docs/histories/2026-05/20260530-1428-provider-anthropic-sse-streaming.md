## [2026-05-30 14:28] | Task: slice 122 — provider Anthropic SSE decoder + streaming ProtocolTransport + streaming-capable System

### Execution Context

- Agent: Claude Opus 4.8
- Base model: claude-opus-4-8
- Runtime: Claude Code (single-session implementation, TDD)
- Linked plan: [`docs/exec-plans/active/2026-05-30-provider-sse-streaming.md`](../exec-plans/active/2026-05-30-provider-sse-streaming.md)
  — Slice B (slice 122) of the provider-SSE-streaming arc (A/B/C). Design doc
  [`docs/design-docs/api-portability.md`](../design-docs/api-portability.md)
  ("Streaming"); spec
  [`docs/product-specs/0017-fake-provider-first-agent-loop.md`](../product-specs/0017-fake-provider-first-agent-loop.md)
  v1.1 "Streaming sink".

### User Query

> 继续实现下一阶段的代码: provider Anthropic SSE decoder + ProtocolTransport::send_streaming
> + streaming-capable Anthropic System, reusing the slice-97 AttemptSink for
> retry/fallback suppression.

### Changes Overview

- Areas: `oran-provider` (new streaming transport seam + Anthropic SSE decoder +
  streaming-capable Anthropic System). No production change outside
  `oran-provider`.
- Key actions:
  - **`AnthropicSseDecoder`** — new internal `src/oran-provider/_impl/anthropic_sse_decoder.{hpp,cpp}`.
    A stateful incremental sibling of `decode_protocol_response`: `consume(event,
    data)` runs a strict Anthropic state machine (`message_start`→usage/model;
    `content_block_start`→open text/thinking/tool_use + `on_tool_start`;
    `content_block_delta`→append + `on_text_delta`/`on_thinking_delta`/`on_tool_delta`;
    `content_block_stop`→finalize, parsing accumulated tool-input JSON;
    `message_delta`→stop_reason + output usage; `message_stop`→assemble `Response`
    + `on_done`). `ping` is ignored; an `error` event maps to `ErrorKind::upstream`;
    malformed payloads / a stream that ends before `message_stop` map to
    `ErrorKind::parsing`. The assembled `Response` is byte-identical to the body
    decoder's output for the equivalent non-streaming body (pinned by a test that
    decodes both and asserts equality). `nlohmann` stays private to the `.cpp`;
    the header is `nlohmann`-free.
  - **Transport seam.** `ProtocolTransport` gains a provider-owned
    `ProtocolSseCallback = void(std::string_view event, std::string_view data)`
    (keeps `oran-http` types off the provider public surface), a non-pure
    `send_streaming(ProtocolHttpRequest, ProtocolSseCallback)` (default returns an
    "unsupported" error, defined out-of-line so the header stays light), and a
    `supports_streaming()` capability query defaulting to `false`.
  - **Streaming-capable Anthropic System.** `ProtocolTransportSystem::send` now
    streams when `request.stream` is set, the protocol is `anthropic_messages`,
    and `transport_->supports_streaming()` is true: it sends `stream=true`, drives
    `send_streaming` feeding the decoder, and returns the assembled `Response`.
    Otherwise it keeps the existing body path (forcing `stream=false`); OpenAI
    Responses stays body-only this arc. The unconditional `request.stream=false`
    force is removed only on the Anthropic streaming branch.
  - **Retry/fallback suppression — verified, no new code.** Slice-97
    `execution::Runtime` already wraps the caller's `EventSink` in a per-attempt
    `AttemptSink`; tests pin both arms for streaming (pre-first-byte failure
    retries; post-first-byte returns `stream_already_emitted`).

### Design Intent

The load-bearing decision is the **`supports_streaming()` capability gate**, a
small addition not in the source plan's decision log. `ProtocolTransport` is the
seam shared by the test fakes and bootstrap's `HttpProtocolTransport`; that real
transport does not implement `send_streaming` until Slice C. If the Anthropic
System honored `request.stream` (default `true`) unconditionally, configured-route
production — and the `test-bootstrap` localhost round trip that asserts the wire
body contains `"stream":false` — would immediately route through the
unimplemented streaming path. Gating on a `supports_streaming()` query (default
`false`) keeps every slice green: the fake streaming transport overrides it to
`true` and exercises the decoder end-to-end, while `HttpProtocolTransport` keeps
the default and stays body-only until Slice C overrides both methods. This is
cleaner than a sentinel-error fallback (no double-send, no error-code coupling)
and adds one honest virtual instead of forcing every transport to implement
streaming.

`send_streaming` is non-pure (a defaulted virtual) for the same slice-safety
reason: a pure virtual would make `HttpProtocolTransport` and the test
`RecordingTransport` abstract, breaking the `oran-bootstrap` build between slices
B and C.

The decoder lives in its own `_impl/` TU rather than being appended to
`protocol_response.cpp` — the plan's compile-budget contingency — so neither
`nlohmann` provider TU grows toward the cap. The decoder shares no code with the
body decoder's anonymous-namespace helpers (it is a separate TU); the small
`anthropic_stop_reason` mapping is duplicated intentionally to keep the decoder
self-contained.

The decoder is fed on whatever executor `ProtocolTransport::send_streaming` posts
events to (slice 121's `oran-http` posts each complete event to the send
coroutine's executor before the callback fires), so in production the decoder and
`EventSink` run on the agent strand, never the curl thread. The decoder local
lives in the System's `send_stream` coroutine frame and outlives every callback
because all events are delivered before `send_streaming` resolves.

### Files Modified

- `include/oran/provider/protocol_transport.hpp` — `ProtocolSseCallback`,
  `ProtocolTransport::supports_streaming()` (inline, default false),
  `ProtocolTransport::send_streaming` declaration; `<functional>` /
  `<string_view>` includes.
- `src/oran-provider/protocol_transport.cpp` — out-of-line default
  `send_streaming`; Anthropic streaming gate + `send_stream` private helper in
  `ProtocolTransportSystem`; include of the decoder.
- `src/oran-provider/_impl/anthropic_sse_decoder.hpp` — new decoder declaration
  (`nlohmann`-free).
- `src/oran-provider/_impl/anthropic_sse_decoder.cpp` — new decoder
  implementation (`nlohmann` private).
- `tests/provider/test_anthropic_sse_decoder.cpp` — 8 decoder cases (new).
- `tests/provider/test_protocol_transport.cpp` — `StreamingTransport` fake +
  `CapturingSink` + 6 streaming-System cases.
- `tests/provider/test_execution.cpp` — pre-first-byte streaming retry case.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/api-portability.md` — status note bumped to slice 122;
  "Streaming" section promoted from a 4-bullet stub to the full contract
  (transport seam + capability gate, decoder state machine, System behavior, and
  the slice-97 AttemptSink-already-suffices note).
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — v1.1 "Streaming
  sink" status note: provider side shipped (slices 121–122); CLI sink wiring is
  slice 123.
- `docs/QUALITY_SCORE.md` — provider-system row + test-framework provider count
  (63/512 → 81/614); corrected the "forces non-streaming" claim; gap list now
  names the OpenAI SSE decoder + CLI streaming sink.
- `docs/STATUS.md` — slice 122; last-history pointer; active exec-plan note
  (Slice B landed, Slice C remains); `oran-provider` 66/528 → 81/614.
- `docs/exec-plans/active/2026-05-30-provider-sse-streaming.md` — Slice 122 (B)
  progress items checked off; capability-gate decision recorded.

No `ARCHITECTURE.md` change (the oran-http row was flipped in slice 121; the
bootstrap narrative is Slice C). No `compile_budget.json` change (the
`oran-provider` triple is unchanged; the new decoder TU measures on par with the
existing `protocol_response.cpp`).

### Validation

- Commands run:
  - `xmake f -m release && xmake build test-provider` — clean (only pre-existing
    `route_profile_used` missing-initializer warnings in `test_fake.cpp` /
    `test_execution.cpp`, not introduced here).
  - `build/.../test-provider "[sse]"` — 14 cases / 79 assertions.
  - `build/.../test-provider "[execution]"` — 9 cases / 61 assertions.
  - `build/.../test-provider` (full) — **81 cases / 614 assertions** (was 66 / 528).
  - `xmake build` (whole tree) — ok; `orangutan` links.
  - `build/.../test-bootstrap` — **72 / 316 unchanged** (capability gate keeps the
    localhost round trip body-only and asserting `"stream":false`).
  - `build/.../test-agent` — **47 / 10618 unchanged**.
- Tests added/changed: `tests/provider/test_anthropic_sse_decoder.cpp` (+8),
  `tests/provider/test_protocol_transport.cpp` (+6), `tests/provider/test_execution.cpp`
  (+1). TDD: decoder + streaming System + streaming-retry each written test-first
  and watched fail (8/8, then 4/6, then the suppression arm) before implementation.
- Bench impact: none (no bench TU touched). A `response_decode` A/B was deferred —
  the equality test already shows the decoder matches the body path, so no perf
  uncertainty surfaced (plan: bench only to resolve uncertainty).
- Compile-budget delta: new `_impl/anthropic_sse_decoder.cpp` (`nlohmann`) measures
  ≈ 7.83 s on this WSL2 box, on par with `protocol_response.cpp` (≈ 7.90 s) — both
  ≈ 2.6 s reference-equivalent, under the `oran-provider` 3.5 s hard cap.
  `protocol_transport.cpp` ≈ 3.6 s WSL2 (no `nlohmann` added). No category change.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: deferred to Slice C (the user-visible REPL streaming lands
  when `cli::StreamingPromptSink` is wired); this slice is provider-internal.
- Next: Slice 123 (C) — `HttpProtocolTransport::send_streaming` (override
  `supports_streaming()` → true), `cli::StreamingPromptSink`, and `AgentPromptRunner`
  wiring (construct the sink when not quiet, pass `&sink` to `run_turn`). Lights up
  spec 0001 AC3. Then the OpenAI Responses SSE decoder follow-up (Slice 124).

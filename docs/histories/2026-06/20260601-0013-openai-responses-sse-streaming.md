## [2026-06-01 00:13] | Task: slice 124 — OpenAI Responses SSE streaming parity

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: none — `docs/STATUS.md` named Slice 124 as a single-slice
  post-arc follow-up to the completed provider-SSE-streaming plan.

### User Query

> Deeply understand the project architecture and grasp the current implementation
> progress before further implementation. Continue iterating the execution plan.

The orientation pass found the provider-SSE-streaming arc complete at slices
121-123 and identified the OpenAI Responses SSE decoder as the immediate
post-arc follow-up.

### Changes Overview

- Areas: `oran-provider` streaming protocol adapter internals and provider tests.
- Key actions:
  - Added internal `OpenAiResponsesSseDecoder` beside `AnthropicSseDecoder`.
  - The decoder forwards `response.output_text.delta`, reasoning deltas, and
    `response.function_call_arguments.*` to the existing `provider::EventSink`.
  - Terminal `response.completed`, `response.incomplete`, and `response.failed`
    events decode their embedded `response` object through
    `decode_protocol_response`, so streaming and body paths share the same
    authoritative assembler.
  - `ProtocolTransportSystem` now selects the streaming path for OpenAI
    Responses when `request.stream` is true and the injected transport reports
    `supports_streaming() == true`.

### Design Intent

Slice 124 keeps OpenAI parity provider-internal. The agent loop, CLI sink, HTTP
transport, retry/fallback suppression, and body decoder already had the right
seams from slices 121-123. Reusing `decode_protocol_response` for the terminal
OpenAI response avoids a second full response assembler and keeps streamed/body
domain output identical for supported response shapes.

Official OpenAI OpenAPI evidence checked during the slice confirmed the current
Responses stream surface: `text/event-stream` returns `ResponseStreamEvent`, with
semantic events such as `response.output_text.delta`,
`response.function_call_arguments.delta`, `response.output_item.added`, and
terminal `response.completed`.

### Files Modified

- `src/oran-provider/_impl/openai_responses_sse_decoder.hpp` (new).
- `src/oran-provider/_impl/openai_responses_sse_decoder.cpp` (new).
- `src/oran-provider/protocol_transport.cpp`.
- `tests/provider/test_openai_responses_sse_decoder.cpp` (new).
- `tests/provider/test_protocol_transport.cpp`.
- `docs/STATUS.md`.
- `docs/ARCHITECTURE.md`.
- `docs/BUILD_SYSTEM.md`.
- `docs/design-docs/agent-platform.md`.
- `docs/design-docs/api-portability.md`.
- `docs/design-docs/tool-runtime.md`.
- `docs/product-specs/0001-core-react-loop.md`.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md`.
- `docs/QUALITY_SCORE.md`.
- `docs/releases/feature-release-notes.md`.
- `docs/histories/2026-06/20260601-0013-openai-responses-sse-streaming.md`.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 124, last-history pointer, provider SSE parity
  summary, and next-slice routing.
- `docs/design-docs/api-portability.md` — streaming contract now includes the
  OpenAI Responses SSE decoder and the two-protocol transport gate.
- `docs/ARCHITECTURE.md`, `docs/BUILD_SYSTEM.md`,
  `docs/design-docs/agent-platform.md`, `docs/design-docs/tool-runtime.md`, and
  specs 0001/0017 — current architecture/progress summaries no longer describe
  provider SSE as Anthropic-only or future work.
- `docs/QUALITY_SCORE.md` — provider/test counts and next-step text.
- `docs/releases/feature-release-notes.md` — user-visible OpenAI Responses
  streaming entry.
- This history entry.

### Validation

- Commands run:
  - `xmake build test-provider` — passed.
  - `xmake run test-provider` — **86 cases / 643 assertions**.
  - `git diff --check` — passed.
  - `make ci` — passed.
- Tests added/changed: +5 provider cases covering OpenAI streamed text parity,
  reasoning/function-call deltas, incomplete terminal responses, upstream error
  events, truncated streams, and the transport gate switching OpenAI Responses
  from body-only to streaming when supported.
- Bench impact: none; this mirrors the existing streaming architecture and did
  not introduce a performance choice needing A/B measurement.
- Compile-budget delta: one provider `_impl` TU with private `nlohmann`; no public
  header weight and no `compile_budget.json` change.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
  (`openai-responses-sse`).

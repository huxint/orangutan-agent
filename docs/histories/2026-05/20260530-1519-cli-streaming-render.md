## [2026-05-30 15:19] | Task: slice 123 — bootstrap HttpProtocolTransport::send_streaming + cli::StreamingPromptSink + AgentPromptRunner wiring

### Execution Context

- Agent: Claude Opus 4.8
- Base model: claude-opus-4-8
- Runtime: Claude Code (single-session implementation, TDD)
- Linked plan: [`docs/exec-plans/completed/2026-05-30-provider-sse-streaming.md`](../../exec-plans/completed/2026-05-30-provider-sse-streaming.md)
  — Slice C (slice 123), the closing slice of the provider-SSE-streaming arc
  (A/B/C). Design doc
  [`docs/design-docs/api-portability.md`](../../design-docs/api-portability.md)
  ("Streaming"); specs
  [`0001-core-react-loop.md`](../../product-specs/0001-core-react-loop.md) (AC3)
  and
  [`0017-fake-provider-first-agent-loop.md`](../../product-specs/0017-fake-provider-first-agent-loop.md)
  v1.1 "Streaming sink".

### User Query

> 继续实现下一阶段的代码 … then: continue implementing the next slice, then
> commit, and merge into the main branch.

The "next slice" after slice 122 (provider-side Anthropic SSE) is Slice C of the
arc: wire streaming end-to-end into the binary and render it on the terminal.

### Changes Overview

- Areas: `oran-cli` (new streaming sink), `oran-bootstrap` (HTTP transport
  streaming + runner wiring), build graph (`oran-cli` → `oran-provider`).
- Key actions:
  - `cli::StreamingPromptSink` — a `provider::EventSink` that renders answer and
    thinking deltas to an injectable `std::ostream` (default `std::cout`, flushed
    per delta for character-by-character output), prints a one-line
    `[tool: <name>]` marker per tool call, terminates the streamed answer line on
    `on_done`, and reports `rendered_answer_text()` for the runner.
  - bootstrap `HttpProtocolTransport` now overrides `supports_streaming()` →
    `true` and implements `send_streaming` over `http::Client::send_streaming`,
    translating each `http::SseEvent` into the provider `ProtocolSseCallback`.
    The shared header/response translation is factored into `to_body_request` /
    `to_protocol_response` helpers reused by both `send` and `send_streaming`.
  - `AgentPromptRunner` constructs a `StreamingPromptSink` for non-quiet
    streaming runs, passes it to `agent::Loop::run_turn`, and clears the
    assembled `PromptRunResult::text` once the answer streamed live so the CLI
    does not print it twice. New `AgentPromptRunnerOptions::stream_out`
    (`std::ostream*`, default `std::cout`) makes the destination injectable.

### Design Intent

Slices 121–122 shipped streaming up to the provider boundary but gated the live
path behind `ProtocolTransport::supports_streaming()` (default `false`) so the
tree stayed green while the bootstrap override and CLI sink were still missing.
Slice C lifts that gate by overriding `supports_streaming()` in the one transport
that can stream (`HttpProtocolTransport`), so configured-route Anthropic runs now
take the streaming path by default — which is the whole point of spec 0001 AC3.

Two deliberate choices:

- **Injectable `std::ostream`, not hard-coded `std::cout`.** The sink and the
  runner both accept an `std::ostream*` (default `std::cout`). This keeps the
  sink unit-testable with an `std::ostringstream` and gives the binary a future
  redirect hook, mirroring how `OperatorPromptSink` accepts scripted answers for
  tests. The header stays `<iosfwd>`-only.
- **Suppress the duplicate final-text print, but only when text actually
  streamed.** `cli::run_async` prints `PromptRunResult::text` after the turn.
  When the sink streamed the answer live, reprinting it would double the output,
  so the runner clears the text — but *only* when `rendered_answer_text()` holds.
  A non-streaming provider (or a body-path turn) fires no text deltas, so the
  runner returns the assembled text and the CLI prints it as before. Gating on
  the sink's own observation (rather than on `stream_`) keeps both paths correct.

Retry/fallback stream-suppression needed no new code: the slice-97
`execution::Runtime` `AttemptSink` already returns `stream_already_emitted` once
a delta has fired. Mid-stream Ctrl-C surfaces as `Error::cancelled` with
`cancellation_phase=provider` (spec 0018) via the existing loop path.

### Files Modified

- `include/oran/cli/streaming_prompt_sink.hpp` (new) — `StreamingPromptSinkOptions`
  + `StreamingPromptSink`.
- `src/oran-cli/streaming_prompt_sink.cpp` (new) — delta rendering + counters.
- `include/oran/cli.hpp` — re-export the new header.
- `xmake/targets.lua` — `oran-cli` gains an `oran-provider` dep (downward:
  interface → composition; no sibling exception needed).
- `src/oran-bootstrap/provider_backend.cpp` — `HttpProtocolTransport`
  `supports_streaming()` + `send_streaming`, `to_body_request` /
  `to_protocol_response` helpers.
- `include/oran/bootstrap/prompt_runner.hpp` — `AgentPromptRunnerOptions::stream_out`
  (`std::ostream*`), `<iosfwd>`.
- `src/oran-bootstrap/prompt_runner.cpp` — construct/pass the streaming sink,
  clear the duplicate text; `quiet_` + `stream_out_` members.
- `tests/cli/test_cli.cpp` — 4 `StreamingPromptSink` cases (text deltas, tool
  marker, thinking enabled/disabled).
- `tests/bootstrap/test_provider_backend.cpp` — new SSE round-trip case through
  `HttpProviderBackend` + capturing sink; existing body case pinned to
  `stream=false`.
- `tests/bootstrap/test_prompt_runner.cpp` — 2 runner cases (streams to injected
  sink + suppresses duplicate text; keeps text when nothing streamed).
- `tests/bootstrap/test_bootstrap.cpp` — binary E2E `--prompt` now answers SSE
  and asserts `"stream":true`; dead `anthropic_response()` removed.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/api-portability.md` — "Streaming" status flipped to slices
  121–123 shipped end-to-end; closing paragraph now describes the bootstrap
  override + CLI sink + runner wiring; top status note updated.
- `docs/ARCHITECTURE.md` — `oran-cli` row (new `oran-provider` dep +
  `StreamingPromptSink`); `oran-bootstrap` row (`HttpProtocolTransport`
  streaming + runner sink wiring).
- `docs/product-specs/0001-core-react-loop.md` — AC3 marked shipped (slice 123).
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — v1.1 "Streaming
  sink" marked shipped end-to-end (slices 121–123).
- `docs/QUALITY_SCORE.md` — CLI + Bootstrap rows and test counts; Provider gap
  trimmed (CLI sink wiring done); Test-framework count line.
- `docs/releases/feature-release-notes.md` — user-visible streaming entry.
- `docs/exec-plans/active/2026-05-30-provider-sse-streaming.md` → moved to
  `completed/` with Slice C boxes checked and the arc-close recorded.
- `docs/STATUS.md` — slice bump, last-history pointer, active-plan = none
  (arc complete), `oran-cli` / `oran-bootstrap` surface counts, slice-123
  narrative.

### Validation

- Commands run:
  - `xmake build test-cli && xmake run test-cli` — **18 cases / 110 assertions**
    (was 14 / 97); the 4 new streaming cases failed first (empty output) then
    passed (TDD red→green).
  - `xmake build test-bootstrap && xmake run test-bootstrap` — **75 cases / 344
    assertions** (was 72 / 316); SSE round-trip + runner-streaming cases failed
    first then passed; the binary E2E regression (plain-JSON mock) was fixed by
    answering SSE.
  - `xmake -j$(nproc)` (incl. `orangutan` binary) + `xmake test` — all 14
    buckets green.
  - `bash scripts/check-deps.sh` — `oran-cli` → `oran-provider` is downward; ok.
- Tests added/changed: +6 cases across `test-cli` (4) and `test-bootstrap` (2),
  plus the body/streaming split in `test_provider_backend.cpp` and the SSE flip
  in `test_bootstrap.cpp`.
- Bench impact: none (no bench touched; no perf question surfaced).
- Compile-budget delta: none. `oran-cli` gains a small new TU + an
  `oran-provider` link dependency; no `nlohmann`/heavy template weight added,
  and `oran-cli` has no `compile_budget.json` category to breach.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none new. The OpenAI Responses SSE decoder (Slice 124)
  remains the noted post-arc follow-up; an interactive (non-scripted) REPL input
  loop remains the CLI gap.
- Linked release note: `docs/releases/feature-release-notes.md`
  (2026-05-30, `provider-sse-streaming`).

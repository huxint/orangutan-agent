## [2026-05-30 11:24] | Task: slice 121 — oran-http SSE streaming (send_streaming + incremental parser)

### Execution Context

- Agent: Claude Opus 4.8
- Base model: claude-opus-4-8
- Runtime: Claude Code (single-session implementation, TDD)
- Linked plan: [`docs/exec-plans/active/2026-05-30-provider-sse-streaming.md`](../exec-plans/active/2026-05-30-provider-sse-streaming.md)
  — Slice A (slice 121), the first of the provider-SSE-streaming arc (A/B/C).
  Design doc [`docs/design-docs/api-portability.md`](../design-docs/api-portability.md);
  spec [`docs/product-specs/0001-core-react-loop.md`](../product-specs/0001-core-react-loop.md) AC3.

### User Query

> 开始实现代码 (start implementing the code) — execute Slice A of the SSE
> streaming plan: the self-contained `oran-http` platform layer.

### Changes Overview

- Areas: `oran-http` (new SSE streaming surface + internal parser),
  `compile_budget.json` / `compile-budget.md` (new `oran-http` category).
- Key actions:
  - **`SseEvent` + `SseEventCallback`.** `<oran/http/client.hpp>` exports a
    stdlib-only `SseEvent { event{"message"}, data }` value plus
    `SseEventCallback = std::function<void(const SseEvent&)>`. The public
    header gains only `<functional>` — no curl, no `<asio.hpp>` (C6).
  - **Incremental parser.** `src/oran-http/_impl/sse_parser.hpp` —
    `detail::SseParser::feed(chunk, on_event)` is a header-only incremental
    `text/event-stream` parser: handles `\n` / `\r\n`, `field: value` (one
    leading space stripped), multi-line `data:` joined with `\n`, blank-line
    dispatch, `event` defaulting to `"message"`, ignores `:`-comment / `id:` /
    `retry:`, and holds partial lines/events across `feed` calls. An event with
    no `data` field is not dispatched (WHATWG grammar).
  - **`Client::send_streaming(BodyRequest, SseEventCallback)`.** Mirrors the
    `send` libcurl bridge but installs a streaming write callback. On the first
    body byte it decides stream-vs-error from status (2xx) + `Content-Type`
    (`text/event-stream`, case-insensitive); a stream feeds the parser and each
    complete `SseEvent` is `asio::post`-ed to the caller's coroutine executor
    before the sink fires, so the parser/sink never run on the curl thread; a
    non-stream response accumulates into `BodyResponse::body` for the caller to
    decode. A 2xx stream resolves with status + headers and an empty body. The
    existing 50 ms `run_multi` cancel poll surfaces mid-stream cancellation.
  - **`configure_easy` refactor.** Parameterized the write callback / write
    data / header sink so the body path (`write_body`) and the streaming path
    (`write_stream`) share one curl-option setup (DRY).
  - **Compile-budget category.** `oran-http` had no `compile_budget.json`
    category (slice 110 omitted it; `check-compile-budget.sh` hard-fails on a
    missing library). Added `oran-http` at `{1.0, 2.0, 2.5}` to the JSON and the
    `compile-budget.md` per-TU table.

### Design Intent

The load-bearing decision is the **parse-on-curl-thread, deliver-off-it**
handoff. libcurl's write callback runs on the blocking executor (a separate
thread); running the SSE decoder or the caller's sink there would violate the
async model (the production sink writes to the agent strand). So the parser
parses bytes on the blocking thread, but every *complete* event is `asio::post`-ed
to the captured completion executor, and the posted lambda owns the event +
callback by value — nothing the curl thread touches outlives the post. Because
events and the final result are posted to the same executor in order, the
caller sees all events before the awaitable resolves (a strand in production;
a single-threaded `io_context` in tests). A test pins this by asserting the
event callback runs on the `io_context` thread, not the blocking pool.

`send_streaming` reuses the existing `BodyResponse` rather than a new streaming
type: a stream returns status + headers with an empty body, and the
error/non-stream path returns the body so the caller decodes it through the
same HTTP-status→category path `send` already uses. This keeps the surface
minimal and lets the provider layer (Slice B) treat error responses with its
existing body decoder.

Two corrections to the source plan, both confirmed against the repo: the
`oran-http` surface is documented in `ARCHITECTURE.md` + `api-portability.md`
(not `io-runtime.md`, which is `oran-io`); and `oran-http` had no compile-budget
category, so this arc *does* touch `compile_budget.json`.

### Files Modified

- `include/oran/http/client.hpp` — `SseEvent`, `SseEventCallback`,
  `Client::send_streaming` declaration; `<functional>` include.
- `src/oran-http/_impl/sse_parser.hpp` — new incremental SSE parser.
- `src/oran-http/client.cpp` — `write_stream` + `StreamState` +
  `response_is_event_stream` + `ascii_iequals`; `configure_easy`
  parameterization; `send_streaming_blocking`; `async_send_streaming_on`;
  `Client::send_streaming` body.
- `tests/http/test_sse_parser.cpp` — 9 parser cases (new).
- `tests/http/test_streaming.cpp` — 3 streaming cases (happy / error-body /
  mid-stream cancel) over a localhost `text/event-stream` server (new).
- `compile_budget.json` — `oran-http` category `{1.0, 2.0, 2.5}`.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — `oran-http` row records `SseEvent` + `send_streaming`
  + the executor handoff; SSE flipped from "planned" to shipped (server/router
  still planned).
- `docs/design-docs/api-portability.md` — status note: slice 121 adds the
  platform SSE transport; provider-side Anthropic decoder + streaming
  `ProtocolTransport` remain planned (Slice B).
- `docs/rules/compile-budget.md` — `oran-http` added to the `oran-async` per-TU
  tier row.
- `docs/STATUS.md` — slice 121; last-history pointer; `Active exec-plan` → the
  SSE streaming plan; `oran-http` 3 / 21 → 15 / 60.
- `docs/exec-plans/active/2026-05-30-provider-sse-streaming.md` — Slice 121 (A)
  progress items checked off.

### Validation

- Commands run:
  - `xmake f -m release && xmake build test-http` — clean (no warnings).
  - `build/.../test-http "[sse]"` — 9 cases / 22 assertions.
  - `build/.../test-http "[streaming]"` — 3 cases / 17 assertions.
  - `build/.../test-http` (full) — **15 cases / 60 assertions** (was 3 / 21).
  - `scripts/measure-tu.sh` — `client.cpp` 3.17 s on this WSL2 box vs
    `oran-async/runtime.cpp` 2.97 s (≈ same tier; ≈ 1.0-1.1 s reference-equiv).
- Tests added/changed: `tests/http/test_sse_parser.cpp` (+9 cases),
  `tests/http/test_streaming.cpp` (+3 cases). TDD: parser + streaming each
  written test-first and watched fail before implementation.
- Bench impact: none (no bench TU touched this slice).
- Compile-budget delta: `client.cpp` grows by the streaming write path +
  `_impl/sse_parser.hpp`; `oran-http` newly categorized at `{1.0, 2.0, 2.5}`,
  well under the 2.5 s hard cap (≈ 1.0-1.1 s reference-equivalent).

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: deferred to Slice C (the user-visible REPL streaming
  lands when `cli::StreamingPromptSink` is wired); this slice is platform-only.
- Next: Slice B (slice 122) — provider Anthropic SSE decoder +
  `ProtocolTransport::send_streaming` + streaming-capable Anthropic `System`,
  reusing slice-97 `AttemptSink` for retry/fallback suppression.

## [2026-06-14 20:33] | Task: streaming tool roundtrip

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local CLI workspace, release xmake build
- Linked plan: none; this is a focused provider/CLI correctness slice

### User Query

> Debug configured-route CLI output corruption, thinking text mixed into answers,
> a `tcache_thread_shutdown(): unaligned tcache chunk detected` crash, and the
> provider 400 that appeared after an approved `DirectoryList` tool call.

### Changes Overview

- Areas: `oran-http`, `oran-provider`, `oran-cli`, bootstrap version/docs.
- Key actions:
  - Serialized streaming SSE event callbacks and final completion through a
    strand on the caller's completion executor before provider decoders or
    terminal sinks see them.
  - Kept Anthropic thinking-block `text_delta` payloads on the thinking stream
    and made the terminal streaming sink suppress thinking by default.
  - Changed Anthropic `tool_result.content` request mapping to send text content
    instead of arbitrary local structured JSON objects; OpenAI Responses keeps
    structured tool output as a JSON string.
  - Added focused regressions for callback serialization, Anthropic streaming
    thinking routing, Anthropic tool-result text fallback, and CLI thinking
    suppression.

### Design Intent

Streaming provider decoders and terminal sinks are stateful side-effect
consumers, so a multi-worker executor must not invoke them concurrently or out of
SSE order. A strand keeps the curl-thread handoff asynchronous while making the
event contract deterministic. Anthropic Messages `tool_result.content` accepts
text or provider content blocks; Orangutan's local `data_json` object is not one
of those provider content blocks, so the adapter preserves operator-readable text
for Anthropic-compatible endpoints and leaves structured JSON bytes to protocols
whose request shape accepts them.

### Files Modified

- `src/oran-http/client.cpp`
- `src/oran-provider/_impl/anthropic_sse_decoder.cpp`
- `src/oran-provider/protocol_request.cpp`
- `include/oran/cli/streaming_prompt_sink.hpp`
- `tests/http/test_streaming.cpp`
- `tests/provider/test_anthropic_sse_decoder.cpp`
- `tests/provider/test_protocol_request.cpp`
- `tests/cli/test_cli.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 247 summary, latest history pointer, live smoke note,
  and refreshed `oran-http` / `oran-provider` / `oran-cli` test counts.
- `docs/ROADMAP.md` - provider portability frontier now includes live
  Anthropic-compatible configured-route tool round-trip evidence.
- `docs/design-docs/api-portability.md` - streaming callback serialization,
  Anthropic thinking-delta routing, and provider-compatible tool-result mapping.
- `docs/design-docs/cli-runtime.md` - `StreamingPromptSink` public surface and
  default thinking suppression.
- `docs/QUALITY_SCORE.md` - current test counts and provider/CLI status notes.
- `docs/releases/feature-release-notes.md` - user-visible release note.

### Validation

- Commands run:
  - `xmake build test-provider`
  - `xmake run test-provider` (88 cases / 664 assertions)
  - `xmake build test-http`
  - `xmake run test-http` (28 cases / 185 assertions)
  - `xmake build test-cli`
  - `xmake run test-cli` (28 cases / 221 assertions)
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `xmake test` (18 / 18 buckets)
  - `make ci`
  - `git diff --check`
- Tests added/changed: `test-http` streaming callback serialization;
  `test-provider` Anthropic SSE thinking routing and Anthropic tool-result text
  mapping; `test-cli` default thinking suppression.
- Bench impact: none; correctness and protocol-shape change, not a measured hot
  path tradeoff.
- Compile-budget delta: no new target/dependency/header-heavy public surface.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`

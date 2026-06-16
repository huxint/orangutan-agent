## [2026-06-16 02:00] | Task: oran-desktop Slice C — bridge + view-model + sink injection

### Execution Context

- Agent: `claude-code`
- Base model: `claude-opus-4-8[1m]`
- Runtime: Claude Code CLI, GCC 16.1 / xmake, WSL2. Test-driven.
- Linked plan: `docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md` (Slice C)

### User Query

> Continue the desktop track in order. Slice C: the always-built `oran-desktop`
> bridge + view-model — bounded UI↔runtime queues, `DesktopEventSink`,
> `ChatViewModel`, cancellation wiring; inject an optional `provider::EventSink*`
> into `AgentPromptRunner`. ≥60% coverage with a fake provider; no Slint.

### Changes Overview

- Areas: `oran-desktop` (new bridge surface), `oran-bootstrap`
  (`AgentPromptRunner` sink injection), `bench/desktop`, build wiring.
- Key actions:
  - New `<oran/desktop/chat_bridge.hpp>` / `chat_bridge.cpp` (all pure C++, no
    Slint include):
    - `UiUpdate` (+ `UiUpdateKind`) — one runtime→UI streamed event.
    - `ChatViewModel` — folds `UiUpdate`s into renderable transcript state
      (`lines()`, `streaming_text()`, `thinking_text()`, `tool_calls()`,
      `status()`, `error_message()`); `submit_user` opens a streaming turn.
    - `DesktopEventSink : provider::EventSink` — translates `on_text_delta` /
      `on_thinking_delta` / `on_tool_start` / `on_done` into `UiUpdate`s handed
      to a `std::function` delivery hook (the thread-marshalling seam); counts
      `updates_delivered()`.
    - `ChatBridge` — owns bounded `async::Channel<std::string>` prompts
      (UI→runtime) + `async::Channel<UiUpdate>` updates (runtime→UI) queues and
      a per-turn `asio::cancellation_signal`; `submit` / `next_prompt`,
      `request_stop` / `cancellation_slot`, `event_sink`, `drain`,
      `updates_dropped` (overflow-drop accounting), `close`.
  - `bootstrap::AgentPromptRunnerOptions` gains optional
    `provider::EventSink* event_sink`; `run_prompt` uses it in place of the CLI
    `StreamingPromptSink` (priority over the terminal sink, runs even when
    quiet). CLI path unchanged when null.
  - `oran-desktop` deps grow `oran-core` → `oran-core`/`oran-async`/`oran-provider`.
  - `bench/desktop`: real `marshal_64_text_deltas` microbench (delta hand-off
    through the bounded queue + view-model fold).
  - Binary version bumped to `2.0.0-slice250`.

### Design Intent

The bridge seam (`docs/DESKTOP.md`) crosses the Slint UI thread and the
`async::Runtime` executor. Slice C builds the always-built, Slint-free half so it
unit-tests in the default build against a fake provider: a sink that runs on the
provider's coroutine (honouring `EventSink`'s "not thread-safe; hop your own
strand" contract) and forwards through a delivery hook, plus a view-model that is
pure value logic. Both bounded queues reuse `async::Channel` per
`async-and-concurrency.md` A5 — its lock-guarded `try_send`/`try_receive` is the
safe thread crossing (UI thread calls `submit`/`drain`; the runtime coroutine
`co_await`s `next_prompt`). Updates that arrive while the runtime→UI queue is full
are dropped and counted rather than blocking the provider — a deliberate
backpressure choice for a live tracer. The runner injection reuses the full loop
runner (tool/permission/transcript/memory) and supplies only a UI-marshalling
sink, exactly as the plan's Decision Log fixed. The Slint shell binding it
together is Slice D.

### Files Modified

- `include/oran/desktop/chat_bridge.hpp` — new bridge surface (UiUpdate,
  ChatViewModel, DesktopEventSink, ChatBridge).
- `src/oran-desktop/chat_bridge.cpp` — implementations.
- `include/oran/desktop/desktop.hpp` — header comment points at chat_bridge.hpp.
- `include/oran/bootstrap/prompt_runner.hpp` — `AgentPromptRunnerOptions::event_sink`.
- `src/oran-bootstrap/prompt_runner.cpp` — `event_sink_` member + sink selection;
  version 250.
- `tests/desktop/test_chat_bridge.cpp` — new bucket (view-model, sink, bridge).
- `tests/bootstrap/test_prompt_runner.cpp` — injected-sink case + CapturingEventSink.
- `bench/desktop/main.cpp`, `bench/desktop/README.md` — delta-marshalling bench.
- `xmake/targets.lua` — `oran-desktop` deps += `oran-async`, `oran-provider`.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 250, last history, active-plan line, frontier
  (Slice C shipped / next = Slice D), `oran-desktop` 15/59 + `oran-bootstrap`
  157/1581 surfaces.
- `docs/ROADMAP.md` — Desktop row: Slices A–C shipped, next = Slice D.
- `docs/ARCHITECTURE.md` — `oran-desktop` row (deps + bridge) and `oran-bootstrap`
  row (injected `event_sink`).
- `docs/DESKTOP.md` — Current State + Architecture dependency paragraph.
- `docs/QUALITY_SCORE.md` — Desktop App status row.
- `docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md` — Slice C
  progress log + Linked Artifacts history.

### Validation

- TDD per piece: wrote each test first, watched it fail for the right reason
  (ChatViewModel/DesktopEventSink assertions failing against deliberate stubs;
  ChatBridge cases red; runner case red on the missing `event_sink` option), then
  implemented to green.
- `xmake run test-desktop` → **15 cases / 59 assertions** pass (was 1 / 1),
  including submit→prompt roundtrip, delta marshalling, overflow-drop counting,
  stop-cancels-pending-wait, and a `FakeProvider` end-to-end stream into the
  view-model.
- `xmake run test-bootstrap` → 157 cases / 1581 assertions pass (was 156 / 1573).
- Full default build (`--desktop=n`) clean; `xmake test` → 19/19 buckets pass.
- `xmake run bench-desktop` runs (`marshal_64_text_deltas` ~4.4 µs).
- `make ci` green.

### Follow-ups

- Next: Slice D — Slint chat UI (input, live transcript, stop) bound to the
  `ChatBridge`; `orangutan --desktop` builds the runner + bridge and opens the
  working chat; spec 0007 acceptance 1–3; manual `--desktop=y` run; release note.
- The single `asio::cancellation_signal` models one in-flight turn; per-turn
  signal lifecycle (reset between turns) is Slice D's concern.

# Desktop App Guide

Read this file when working on the desktop app surface (`oran-desktop`). It covers the
framework choice, the in-process architecture, local dev, build, conventions, and
testing strategy.

## Current State

- **Framework**: [Slint](https://slint.dev) — declarative `.slint` markup compiled to
  C++; GPU-accelerated renderer; first-class animations; low memory footprint.
- **Model**: **in-process, local-only.** `oran-desktop` links into the `orangutan`
  binary and drives `agent::Loop` directly — no HTTP server, no SSE over the wire, no
  auth token. (The old browser Web UI / `cpp-httplib` server is gone.)
- **Status**: in progress (chat-tracer plan). Slices A–C have shipped: the gated
  prebuilt `slint` package, the `.slint`→C++ codegen rule, a skeleton
  `orangutan --desktop` window (A), the `web`→`desktop` config migration (B), and
  the always-built `oran-desktop` bridge / view-model — `ChatViewModel`,
  `DesktopEventSink`, and a `ChatBridge` with bounded UI↔runtime queues and
  per-turn cancellation, plus an injected `provider::EventSink*` on
  `AgentPromptRunner` (C, `<oran/desktop/chat_bridge.hpp>`). Slice 251 adds the
  always-built runtime-side glue — `run_chat_session` (the session loop) with
  per-turn `begin_turn()` cancellation, and `async::Runtime::start()` so the
  runtime runs alongside the Slint loop. The live chat view (Slint UI binding
  input, streamed transcript, and stop to the bridge) plus the `orangutan
  --desktop` launch wiring land in Slice D. See
  [`product-specs/0007-web-ui.md`](product-specs/0007-web-ui.md)
  and [`exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md`](exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md).

## Architecture

The one genuinely new design seam is the **Slint event loop ↔ asio executor bridge**:

- Slint owns the UI thread and its own event loop. The agent runtime lives on
  `oran::async::Runtime`'s executor.
- Prompts entered in the UI are posted onto the runtime executor; the runner drives
  `agent::Loop` exactly as `oran-cli`'s `AgentPromptRunner` does.
- Streamed model output (text/thinking/tool deltas) flows back to the UI through the
  existing `provider::EventSink` — the same observer `cli::StreamingPromptSink`
  implements — marshalled onto the UI thread via Slint's event-loop post/invoke API.
- Cancellation (the chat "stop" control) emits on the turn's `asio::cancellation_signal`,
  surfacing as `Error::cancelled` with `cancellation_phase=provider_stream`
  after visible deltas.
- Backpressure: the bridge uses bounded queues for UI→runtime and runtime→UI traffic,
  per the platform's "bounded queues are the default" rule.

At the target state `oran-desktop` depends on `oran-agent` and
`oran-orchestration` (for the conversation-DAG view); it does **not** depend on
`oran-http` (that is an outbound client, irrelevant to an in-process GUI). The
Slice-A skeleton depended only on `oran-core`; Slice C added `oran-async` (the
bounded `async::Channel` queues) and `oran-provider` (the `EventSink` the bridge
implements). The agent runtime couples in through `oran-bootstrap`, which depends
on `oran-desktop` and injects the bridge's `DesktopEventSink` into its own
`AgentPromptRunner` — so the desktop library never depends on `oran-bootstrap`
(no cycle), and the `agent::Loop` drive stays on bootstrap's side of the seam.

## Local Dev

```sh
# Configure with the desktop shell on (downloads + installs the prebuilt Slint
# package the first time; off by default so a normal build pays nothing for it).
xmake f -m release --desktop=y
# Build and run the app
xmake build orangutan
xmake run orangutan -- --desktop
```

A build configured without `--desktop=y` still compiles the always-built
`oran-desktop` surface and `test-desktop`, and `orangutan --desktop` exits with a
"rebuild with `--desktop=y`" error instead of opening a window.

For UI-only iteration, edit the `.slint` markup under `src/oran-desktop/ui/` and rebuild;
the Slint compiler regenerates the C++ the library includes.

## Conventions

- **Markup**: UI is declared in `.slint` files; keep view-models (the C++ that feeds the
  UI and handles callbacks) thin and testable, separate from the runtime bridge.
- **Animations**: use Slint's `animate` / `states` / `transitions` constructs; gate them
  behind the `desktop.reduce_motion` config for accessibility.
- **Theme**: a single default theme with light/dark following the system
  (`desktop.theme`); minimal custom styling.
- **Generated code stays contained**: the `.slint`-generated C++ lives only inside
  `oran-desktop` (module-boundary + compile-budget rules).
- **No web tech**: no HTML/CSS/JS, no embedded browser/webview.

## Testing

- `tests/desktop/` exercises the bridge logic and view-model wiring (prompt submission,
  streaming-delta marshalling, cancellation, session loading) with a fake provider —
  not pixel rendering, which Slint owns.
- Coverage target: ≥ 60% (see [`rules/testing-and-bench.md`](rules/testing-and-bench.md)).
- Visual regression: out of scope for v1.

## See Also

- [`product-specs/0007-web-ui.md`](product-specs/0007-web-ui.md) — Desktop App spec.
- [`design-docs/agent-platform.md`](design-docs/agent-platform.md) — interfaces + vision.
- [`rules/libraries.md`](rules/libraries.md) — Slint dependency + license note.
- [`RELIABILITY.md`](RELIABILITY.md) — in-app status panel + structured-log metrics.

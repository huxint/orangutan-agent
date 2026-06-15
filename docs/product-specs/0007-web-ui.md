# 0007 — Desktop App

## User Problem

Operators want a native desktop app for: live chat with the agent, session browsing,
admin operations, and inspecting orchestration DAGs and automation jobs. The legacy
`orangutan/` exposed this through a browser Web UI; v2 replaces that with a **local,
in-process desktop application** — modern, animated, and low-memory — instead of a
networked web server.

## Approach

- **Framework:** [Slint](https://slint.dev) — C++ is a first-class API (`.slint` markup
  is compiled to C++ that the library `#include`s), with a GPU-accelerated renderer,
  first-class animations/transitions, and a very low memory footprint. It links like a
  normal library and fits the single-binary C++26 architecture and the compile-budget
  discipline far better than a heavyweight toolkit such as Qt.
- **In-process, local-only.** `oran-desktop` is an interface-layer library (a peer of
  `oran-cli`). It drives `agent::Loop` directly on the shared `oran::async::Runtime`
  executor through the same bootstrap path the CLI uses — no HTTP server, no SSE over
  the wire, no auth token. Streamed model output reaches the UI through the existing
  `provider::EventSink` (the same mechanism `cli::StreamingPromptSink` uses).
- **No network surface.** The desktop app is launched with `orangutan --desktop`. Remote
  access is intentionally out of scope; channels remain the runtime's remote reach.

## Scope (v1)

- `oran-desktop` library: a Slint UI shell plus the **Slint event-loop ↔ asio executor
  bridge** (the one new design seam — documented in [`../DESKTOP.md`](../DESKTOP.md)).
- Panels (native Slint views):
  - **Chat** — send a prompt; render streamed tokens live; a stop control that cancels
    the in-flight turn.
  - **Sessions** — list sessions; load a session into the chat view.
  - **Audit / admin** — recent tool calls + permission decisions with timestamps and
    identity.
  - **Automation** — list automation jobs; schedule / modify a job.
  - **Status** — uptime, active agents, token/cost counters (replaces the old
    `/healthz` + `/metrics` HTTP endpoints; see [`../RELIABILITY.md`](../RELIABILITY.md)).
- Theme + accessibility: a default theme with light/dark following the system, and a
  reduce-motion toggle.

## Scope (v1.1)

- Conversation-DAG renderer for orchestration teams (the debugging surface
  [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md) assigns to the
  desktop app).
- Admin views for hooks (list + test-fire).
- Richer attachment rendering for structured tool output (spec
  [`0014`](0014-structured-tool-output.md)).

## Scope (v2)

- Polished motion design and transitions across views.
- Additional themes.
- Optional local IPC seam if a detached/headless companion is ever needed (would be a
  deliberate, separately-specced reintroduction of a local endpoint).

## Out Of Scope

- **Remote / browser access.** The app is local-first and in-process; there is no
  network listener. A self-hosted team's remote reach is via channels, not a GUI.
- Mobile / responsive layouts.
- Web technologies (HTML/CSS/JS, embedded browser/webview).

## Config

The desktop app uses a small UI-preferences config block (shipped in slice 249):

```json
"desktop": { "enabled": false, "theme": "system", "reduce_motion": false }
```

`oran-config` parses this as `DesktopConfig { enabled, theme, reduce_motion }`
(`theme` ∈ {`system`, `light`, `dark`}; unknown fields warn). It replaced the legacy
networked `web` block (`WebConfig { enabled, bind, port }`) from the removed browser
Web UI, which is gone.

## Acceptance Criteria

1. `orangutan --desktop` opens the app window and reaches the chat view.
2. A submitted prompt renders streamed tokens in real time; the stop control cancels the
   in-flight turn (`Error::cancelled`, `cancellation_phase=provider_stream` after visible deltas).
3. The streamed view survives a slow tool call without dropping output.
4. The audit view shows recent tool calls + permission decisions with timestamps and
   identity.
5. Resident memory stays within the documented low-footprint target for an idle app
   (concrete figure set when `oran-desktop` is built and benched).
6. `tests/desktop/` ≥ 60% coverage (UI-bridge logic + view-model wiring; rendering
   itself is exercised by Slint).

## Design Doc Cross-References

- [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)
- [`../DESKTOP.md`](../DESKTOP.md)

## Risks

- Slint is triple-licensed and has a younger ecosystem than Qt; acceptable for a
  focused, custom UI. Recorded in [`../rules/libraries.md`](../rules/libraries.md) and
  [`../SUPPLY_CHAIN_SECURITY.md`](../SUPPLY_CHAIN_SECURITY.md).
- A GUI toolkit adds build cost; mitigated by confining the `.slint`-generated code to
  `oran-desktop` and giving it its own
  [`../rules/compile-budget.md`](../rules/compile-budget.md) row.
- Bridging the Slint event loop and the asio executor must respect cancellation and
  backpressure; covered in [`../DESKTOP.md`](../DESKTOP.md).

## Validation

```sh
xmake build oran-desktop
xmake test test-desktop
xmake run orangutan -- --desktop
```

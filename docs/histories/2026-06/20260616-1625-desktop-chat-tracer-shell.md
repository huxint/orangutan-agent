## [2026-06-16 16:25] | Task: oran-desktop Slice D — chat tracer end-to-end (gated Slint shell + bootstrap launch)

### Execution Context

- Agent: `claude-code`
- Base model: `claude-opus-4-8[1m]`
- Runtime: Claude Code CLI, GCC 16.1 / xmake, WSL2.
- Linked plan: `docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md` (Slice D)

### User Query

> Continue from the slice-251 checkpoint: build the gated Slint shell + bootstrap
> launch ("go"). The interactive acceptance (live streaming + stop) is the
> operator's smoke; this slice delivers the implementation.

### Changes Overview

- Areas: `oran-desktop` (gated Slint shell), `oran-bootstrap` (`--desktop` launch).
- Key actions:
  - `ui/app_window.slint` — replaced the Slice-A placeholder with the chat view:
    a `ScrollView`/`VerticalLayout` transcript over an `[ChatLineData]` model,
    a status line, an input `LineEdit`, and Send/Stop buttons. Exposes `lines`,
    `status-text`, `streaming`, `dark`, two-way `input-text`, and `send`/`stop`
    callbacks for C++.
  - `shell.hpp` — `RunOptions` grows `{ ChatBridge* bridge; bool dark; bool
    reduce_motion; }` (forward-declares `ChatBridge`, no Slint leak).
  - `shell/shell.cpp` — binds `send` → `view_model.submit_user` + `bridge.submit`,
    `stop` → `bridge.request_stop`; a 33 ms repeating `slint::Timer` drains the
    bridge into a UI-thread `ChatViewModel` and mirrors it into the Slint
    properties; closes the bridge input on window close.
  - `bootstrap.cpp` — `run_desktop` (gated `ORAN_ENABLE_DESKTOP`): loads config,
    builds the `async::Runtime` + a focused `RuntimeAssembly`, resolves the
    provider (live `HttpProviderBackend` when a route resolves, else a scripted
    `FakeProvider` fallback so the window streams offline), creates the
    `ChatBridge`, builds `AgentPromptRunner` with `.event_sink =
    bridge.event_sink()` + `.quiet = true`, defines a `TurnRunner` that calls
    `run_prompt` (and finalizes the UI with `on_done` on a turn error/cancel),
    `Runtime::start()`s, co-spawns `run_chat_session`, runs the shell, then tears
    down deterministically. The `--desktop` dispatch now calls `run_desktop`.
  - Binary version bumped to `2.0.0-slice252`.

### Design Intent

This completes the bridge seam (`docs/DESKTOP.md`): Slint owns the main thread
via `window->run()`, while the agent runs on the `async::Runtime`'s own workers
(`Runtime::start()`, slice 251) and the lock-guarded `ChatBridge` channels cross
between them — the UI timer only ever calls the thread-safe `drain`. The runner
is reused verbatim (tool/permission/transcript/memory) with a UI-marshalling
sink injected, exactly as the plan's Decision Log fixed; the desktop adds no
agent logic. The scripted `FakeProvider` fallback (per the slice-D provider
decision) keeps `--desktop` demonstrable offline. Teardown is deterministic
because `Runtime::stop()` does **not** join the io workers (only the destructor
does): on window close the shell closes the bridge input, then `run_desktop`
cancels any in-flight turn and **waits on a completion promise** for
`run_chat_session` to finish before stopping the runtime and dropping the
runner the session borrows — avoiding a use-after-free.

### Files Modified

- `src/oran-desktop/ui/app_window.slint` — chat view markup (replaces skeleton).
- `include/oran/desktop/shell.hpp` — `RunOptions` (bridge + UI prefs).
- `src/oran-desktop/shell/shell.cpp` — callbacks + drain timer + run loop.
- `src/oran-bootstrap/bootstrap.cpp` — `run_desktop` launch + dispatch; `kVersion`
  → `2.0.0-slice252`; includes (`<oran/desktop/chat_bridge.hpp>`,
  `<oran/provider/fake.hpp>`, `<future>`).

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/DESKTOP.md` — Current State: chat tracer shipped (gated UI + launch).
- `docs/product-specs/0007-web-ui.md` — Acceptance Criteria status (delivered).
- `docs/ARCHITECTURE.md` — `oran-desktop` row: gated shell + bootstrap launch.
- `docs/STATUS.md` — slice 252, last-history pointer, frontier.
- `docs/ROADMAP.md` — Desktop row: chat tracer shipped; next = post-chat panels.
- `docs/QUALITY_SCORE.md` — Desktop App row.
- `docs/releases/feature-release-notes.md` — `orangutan --desktop` chat tracer row.
- `docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md` — Slice D progress.

### Validation

- Default build (`--desktop=n`) clean; full `xmake test` → **19/19 buckets pass**
  (the gated `run_desktop` is `#if`-excluded; no test-count change this slice).
- Gated build (`--desktop=y`) clean: `slint-compiler` codegen of the new chat
  markup, `shell.cpp` compiles against the generated bindings, `run_desktop`
  compiles and links `libslint_cpp.so`.
- Startup smoke (`xmake run orangutan -- --desktop`, ~8 s timeout): the binary
  loads config, assembles runtime/provider/runner/bridge, co-spawns the session,
  and enters the Slint event loop with the window open — no errors (exit 124 =
  still running at timeout). **Acceptance criterion 1 (window opens → chat view)
  verified.**
- **Pending operator smoke:** acceptance criteria 2–3 (a submitted prompt streams
  tokens live; the stop control cancels the in-flight turn; slow tool calls don't
  drop output) need an interactive session on a display with a configured
  provider — they cannot be driven headlessly here.

### Follow-ups

- Operator: run `xmake run orangutan -- --desktop` and confirm acceptance 2–3,
  then move the chat-tracer exec-plan to `completed/`.
- Polish: on a cancelled turn the status line currently resets via a synthetic
  `on_done`; consider a dedicated cancelled state. Auto-scroll-to-bottom and
  theme depth (Palette) are deferred.
- Next desktop slice: post-chat panels (sessions / audit / orchestration DAG) per
  spec 0007 v1.

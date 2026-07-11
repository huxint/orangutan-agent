# oran-desktop — First Slice: Slint Chat Tracer

> **Closed 2026-06-16 (complete).** All five milestones shipped in slices 248–252:
> Slint packaging + gated skeleton (248), `web`→`desktop` config migration (249),
> always-built bridge + view-model + sink injection (250), `Runtime::start()` +
> session driver (251), and the gated Slint chat tracer + `orangutan --desktop`
> launch (252). Spec 0007 acceptance 1 (window opens → chat view) is verified by
> the build's startup smoke; acceptance 2–3 (live streaming + stop) were closed by
> the operator smoke that gated this archival.
>
> Status: **completed** · Date: 2026-06-14 · Type: new-library + new-dependency ·
> Owner: huxint
>
> First implementation slice of the Desktop App track
> ([`../../ROADMAP.md`](../../ROADMAP.md) Desktop row). Design is already settled
> in [spec 0007](../../product-specs/0007-web-ui.md) and
> [`../../DESKTOP.md`](../../DESKTOP.md); this plan stages the build-out and records
> the three architecture decisions those docs did not yet pin down (target split,
> Slint packaging, sink injection — see Decision Log).

## Goal

Ship a working `orangutan --desktop` chat experience: a native Slint window where an
operator types a prompt, sees model tokens stream in live, and can press **stop** to
cancel the in-flight turn. The agent runs in-process on the shared `async::Runtime`
through the same `agent::Loop` path the CLI uses — no HTTP server, no auth. Land it
without regressing the default build's compile budget: the GUI toolkit and Slint-
generated code are opt-in (`xmake f --desktop=y`), while the bridge/view-model logic is
always built and unit-tested against a fake provider.

## Scope

- **In scope:**
  - New library `oran-desktop` (pure C++, always built): the Slint↔asio bridge, bounded
    UI↔runtime queues, `DesktopEventSink : provider::EventSink`, and a `ChatViewModel`.
  - New library `oran-desktop-shell` (gated `--desktop=y`): the `.slint` UI + generated
    C++ + window entry point.
  - A custom xmake `slint` package (prebuilt C++ binaries) + a `.slint` → C++ codegen
    build rule confined to `oran-desktop-shell`.
  - A `--desktop` build option and the `orangutan --desktop` runtime flag wired through
    `oran-bootstrap` (with a clear "rebuild with `--desktop=y`" error in default builds).
  - Injecting an optional `provider::EventSink*` into `bootstrap::AgentPromptRunner` so
    the desktop reuses the full loop-driving runner instead of duplicating it.
  - `web` → `desktop` config-block migration (`DesktopConfig{enabled,theme,reduce_motion}`).
  - `tests/desktop/` + `bench/desktop/` buckets (C12 parity); ≥60% bridge coverage.
  - Docs synced per the Prime Directive: `DESKTOP.md`, spec 0007 acceptance,
    `libraries.md`, `SUPPLY_CHAIN_SECURITY.md`, `ARCHITECTURE.md`, `STATUS.md`,
    `ROADMAP.md`, `BUILD_SYSTEM.md`, and the tech-debt tracker row.

- **Out of scope (later desktop slices / v1.1+):**
  - Sessions, Audit, Automation, Status panels (spec 0007 v1 list beyond Chat).
  - Conversation-DAG renderer, hooks admin, rich structured-output attachments.
  - Polished motion design, additional themes, visual-regression testing.
  - Any remote/network surface.

## Context

- **Design docs:** [spec 0007](../../product-specs/0007-web-ui.md),
  [`../../DESKTOP.md`](../../DESKTOP.md) (bridge seam),
  [`../../design-docs/agent-platform.md`](../../design-docs/agent-platform.md).
- **Code seams to reuse / extend:**
  - `provider::EventSink` (`include/oran/provider/system.hpp`) — the streaming observer;
    `cli::StreamingPromptSink` is the terminal analog the desktop sink mirrors.
  - `bootstrap::AgentPromptRunner` (`include/oran/bootstrap/prompt_runner.hpp`) — owns
    tool registry, permissions, transcript tail, approval sink, memory framing. Gains an
    optional injected sink.
  - `async::Runtime` (`include/oran/async/runtime.hpp`) — `executor()`,
    `cpu_executor()`, `make_strand()` for the bridge.
  - `oran-bootstrap` `run()` mode dispatch (`src/oran-bootstrap/bootstrap.cpp`) — where
    `--desktop` selects the desktop launch instead of `cli::run_async`.
  - Build wiring: `oran_lib(...)` helper + `has_config(...)` gating in
    `xmake/targets.lua`; `oran_test(...)` in `xmake/tests.lua`; option pattern in
    `xmake/options.lua`; package list in `xmake/packages.lua`.
  - Config: `WebConfig` + `parse_web` (`include/oran/config/config.hpp`,
    `src/oran-config/config.cpp`), `config.example.json`, `tests/config`.
- **Constraints:** C2 (no `std::thread` — bridge uses the runtime), C11 (cancel-aware),
  C12 (tests+bench buckets), C14 (≤600 LoC/6 files per slice), C15/L1 (new dep documented
  before the packages edit), C16 (docs in sync), C17 (C++26 idioms, no `<iostream>` in
  `src/`).
- **Compile-budget impact:** Slint is the heaviest dep in the tree. Confined to the gated
  `oran-desktop-shell`; default `xmake` build links neither Slint nor generated code.
  `oran-desktop-shell` gets its own compile-budget row. Prebuilt binaries mean no Rust
  toolchain in any build path.

## Risks

- **Risk:** xmake-repo has no `slint` package (verified 2026-06-14). **Mitigation:**
  Slice A writes a custom `package("slint")` consuming the official prebuilt C++ release
  (headers + `slint-compiler` + runtime lib); front-loaded so integration failure
  surfaces first, before any streaming work depends on it.
- **Risk:** the `.slint` → C++ codegen step must run before the shell library compiles and
  stay confined to that library. **Mitigation:** an xmake build rule invoking
  `slint-compiler`; generated output under the shell target's build dir only; the rest of
  the tree never includes it.
- **Risk:** bridging the Slint event loop and the asio executor must respect cancellation
  and backpressure. **Mitigation:** bounded queues both directions; cancellation emits on
  the turn's `asio::cancellation_signal` surfacing `Error::cancelled`
  `cancellation_phase=provider_stream`; all queue/marshalling logic lives in the
  always-built bridge and is unit-tested with a fake provider (no Slint needed).
- **Risk:** docs claim "no web" while config still parses `web`. **Mitigation:** Slice B
  completes the `web`→`desktop` migration and clears the tech-debt row.
- **Risk:** Slint license (GPLv3 / royalty-free / commercial). **Mitigation:** recorded in
  `libraries.md` (moved to the Optional/Feature-Gated table) + `SUPPLY_CHAIN_SECURITY.md`.

## Milestones

Each milestone is one slice (≤ ~600 LoC / ~6 files) landing one history entry, with its
docs synced in the same commit (Prime Directive).

1. **Slice A — Slint packaging + skeleton window (gated).** Custom `slint` package;
   `--desktop` build option; `oran-desktop-shell` target + `.slint` codegen rule; a window
   `orangutan --desktop` opens; `--desktop` flag parsed in bootstrap with a default-build
   "rebuild with `--desktop=y`" error. Proves the integration unknown. Docs: `libraries.md`,
   `SUPPLY_CHAIN_SECURITY.md`, `BUILD_SYSTEM.md`, `DESKTOP.md` build section, `ARCHITECTURE.md`.
2. **Slice B — `web` → `desktop` config migration.** `DesktopConfig{enabled,theme,
   reduce_motion}` replacing `WebConfig`; `parse_desktop`; `config.example.json`;
   `tests/config`; bootstrap summary line. Clears the tech-debt row. No Slint.
3. **Slice C — bridge + view-model (always-built `oran-desktop`).** Bounded UI↔runtime
   queues, `DesktopEventSink`, `ChatViewModel`, cancellation wiring; inject optional
   `provider::EventSink*` into `AgentPromptRunner`; `tests/desktop` + `bench/desktop`
   buckets; ≥60% coverage with a fake provider.
4. **Slice D — chat tracer end-to-end (gated shell).** Slint chat UI (input, live
   transcript, stop) bound to the bridge; `orangutan --desktop` builds the runner + bridge
   and opens the working chat. Satisfies spec 0007 acceptance criteria 1–3; manual
   `--desktop=y` run recorded.

## Validation

- **Commands (default build — Slint absent):** `xmake f -m release && xmake -j$(nproc)`;
  `xmake test` (includes `test-desktop` bridge coverage); `make ci`.
- **Commands (desktop build):** `xmake f -m release --desktop=y && xmake -j$(nproc)`;
  `xmake build oran-desktop-shell`; `xmake run orangutan -- --desktop`.
- **Manual checks (Slice D):** window opens to chat; a submitted prompt streams tokens
  live; stop cancels the in-flight turn (`Error::cancelled`,
  `cancellation_phase=provider_stream` after visible deltas); a slow tool call does not
  drop streamed output.
- **Observability checks:** trace row records the desktop turn (`origin=desktop`);
  cancellation phase recorded as above.
- **Bench comparison:** `bench/desktop` placeholder at first; a bridge-marshalling
  microbench once there is a meaningful workload to measure.

## Progress Log

- [x] 2026-06-14: Confirm scope, decisions, CI constraints; verify xmake-repo has no
      `slint` package; scaffold this plan.
- [x] 2026-06-15 (slice 248): Slint package + gated shell + skeleton window;
      bootstrap `--desktop` flag (gated launch / ungated "rebuild with `--desktop=y`"
      error); `tests/desktop` + `bench/desktop` buckets; build/library/supply-chain/
      architecture/roadmap docs synced. Both build paths verified (default:
      `test-desktop` passes + graceful error; gated: `slint-compiler` codegen + shell
      compile + `libslint_cpp.so` link + test passes). Live window confirmed:
      `orangutan --desktop` runs the Slint event loop on WSLg/Xwayland under both the
      default and `winit-software` renderers, no backend/font errors.
- [x] 2026-06-16 (slice 249): `web`→`desktop` config migration — `DesktopConfig{enabled,
      theme, reduce_motion}` + `parse_desktop` (theme ∈ {system,light,dark}, unknown-field
      warnings) replacing `WebConfig`/`parse_web`; `config.example.json`; `tests/config`
      (56 cases / 537 assertions); bootstrap `desktop=` summary line. Tech-debt row closed.
- [x] 2026-06-16 (slice 250): bridge + view-model + sink injection — always-built
      `oran-desktop` gains `ChatViewModel` (folds `UiUpdate`s into transcript
      state), `DesktopEventSink : provider::EventSink` (translates streamed deltas
      into `UiUpdate`s through a delivery hook), and `ChatBridge` (bounded
      `async::Channel<std::string>` prompts + `async::Channel<UiUpdate>` updates
      queues, per-turn `asio::cancellation_signal`, overflow-drop accounting,
      sink delivery wired into the runtime→UI queue); `AgentPromptRunnerOptions`
      gains an optional injected `provider::EventSink*` (priority over the
      terminal sink, runs even when quiet). `oran-desktop` deps grow to
      `oran-core`/`oran-async`/`oran-provider`. Built test-first vs a fake
      provider: `test-desktop` 15 cases / 59 assertions (incl. submit→prompt
      roundtrip, delta marshalling, overflow-drop, stop-cancels-wait, and a
      `FakeProvider` end-to-end stream), `test-bootstrap` 157 / 1581;
      `bench/desktop` gains a delta-marshalling microbench. No Slint.
- [x] 2026-06-16 (slice 251): Slice D always-built core — `async::Runtime::start()`
      (non-blocking launch sharing `run()`'s worker-spawn + idle→running→stopped
      state machine, so the runtime coexists with the Slint loop; threads stay
      inside `Runtime`, A3) and `desktop::run_chat_session` (runtime-side session
      loop: `next_prompt` → embedder `TurnRunner` → stream, with a fresh per-turn
      `begin_turn()` cancellation slot and `request_stop()` posting its emit onto
      the runtime executor). Root-cause fix: `ChatBridge::close()` is input-only so
      a final in-flight turn's deltas stay drainable. Test-first: `test-async`
      16/83, `test-desktop` 17/70; full `xmake test` 19/19. Docs synced
      (async-model, ARCHITECTURE, DESKTOP, STATUS, ROADMAP).
- [x] 2026-06-16 (slice 252): Slice D gated shell — `ui/app_window.slint` chat UI
      (transcript, input, Send/Stop) bound to `ChatBridge` via `shell::run` (a
      `slint::Timer` drains the runtime→UI queue into a `ChatViewModel`);
      `orangutan --desktop` launch (`bootstrap::run_desktop`): config → runtime +
      `RuntimeAssembly` → provider (live `HttpProviderBackend`, else scripted
      `FakeProvider` fallback) → `AgentPromptRunner` (injected `event_sink`) →
      bridge → `Runtime::start()` → co-spawn `run_chat_session` → window;
      deterministic teardown via a completion-promise wait. Gated `--desktop=y`
      build clean; default build + `xmake test` 19/19; startup smoke opens the
      window (acceptance 1). Operator smoke of live streaming + stop (acceptance
      2–3) confirmed; spec 0007 acceptance 1–3 closed and this plan archived to
      `completed/`.
- [x] Per slice: update invalidated docs in the same commit (`docs/rules/docs-in-sync.md`).
- [x] Per milestone: verify the affected targets and refresh current-contract docs.
- [x] Update `docs/QUALITY_SCORE.md` Desktop App row when the library lands.
- [x] Release note when `orangutan --desktop` becomes user-visible (Slice D, slice 252).

## Decision Log

- 2026-06-14: **Two-target split** — `oran-desktop` (pure-C++ bridge/view-model, always
  built + tested) + `oran-desktop-shell` (Slint UI, gated `--desktop=y`). Keeps the ≥60%
  bridge coverage in default CI and keeps Slint's compile/link cost opt-in, honoring
  rule L4 / the compile budget. Mirrors the `channel_qq` separate-target precedent.
- 2026-06-14: **Slint via prebuilt C++ binaries** through a custom `package("slint")`,
  not a CMake/Rust source build. Avoids a Rust toolchain in any build path and keeps the
  generated-code compile cost the only desktop tax. `slint` moves to the Optional/
  Feature-Gated table in `libraries.md`.
- 2026-06-14: **Inject `provider::EventSink*` into `AgentPromptRunner`** rather than give
  `oran-desktop` its own loop-driving runner. The runner's tool/permission/transcript/
  approval/memory logic is reused verbatim; the desktop supplies a UI-marshalling sink.
  `oran-bootstrap` owns the `--desktop` launch and depends on the desktop targets; the
  desktop libraries never depend on bootstrap (no cycle).
- 2026-06-14: **First deliverable is the full chat tracer** (window + live streaming +
  stop), with Slint packaging proven as Slice A — chosen over a minimal echo-window first
  step (user decision).

## Linked Artifacts

- Related design doc: `docs/DESKTOP.md`, `docs/design-docs/agent-platform.md`.
- Related product spec: `docs/product-specs/0007-web-ui.md` (Desktop App).
- Brainstorm/origin: this session (2026-06-14).
- PRs: _(filled per slice)_
- History entry: Slice A — `docs/histories/2026-06/20260615-2300-oran-desktop-slice-a.md`;
  Slice B — `docs/histories/2026-06/20260616-0015-desktop-config-migration.md`;
  Slice C — `docs/histories/2026-06/20260616-0200-desktop-bridge-view-model.md`;
  Slice D core — `docs/histories/2026-06/20260616-1448-async-runtime-start-desktop-session.md`;
  Slice D shell — `docs/histories/2026-06/20260616-1625-desktop-chat-tracer-shell.md`.
- Release note: _(Slice D)_

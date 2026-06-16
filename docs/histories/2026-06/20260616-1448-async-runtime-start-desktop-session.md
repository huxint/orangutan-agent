## [2026-06-16 14:48] | Task: async Runtime::start() + always-built desktop session driver (Slice D core)

### Execution Context

- Agent: `claude-code`
- Base model: `claude-opus-4-8[1m]`
- Runtime: Claude Code CLI, GCC 16.1 / xmake, WSL2. Test-driven.
- Linked plan: `docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md` (Slice D, always-built core)

### User Query

> Continue the prior conversation (which had started Desktop Slice D and died
> mid-flight on API errors, leaving uncommitted WIP). Resume from where it left
> off and package the work.

The prior session had written tests + code for two always-built Slice-D pieces
but never saw the test result (the API dropped right after running them); one
session-driver test was in fact failing. This slice diagnoses and fixes that
failure, then lands the verified always-built core as its own commit ahead of the
gated Slint shell (user chose "checkpoint core, then shell").

### Changes Overview

- Areas: `oran-async` (non-blocking `Runtime::start()`), `oran-desktop`
  (always-built session driver + per-turn cancellation), their test buckets.
- Key actions:
  - `async::Runtime::start()` — spawns the io workers and returns immediately
    (vs `run()`, which joins), so a foreign event loop (the Slint desktop shell)
    can own the calling thread while the runtime drives coroutines on its own
    pool. `run()` and `start()` share a new private `spawn_workers()` that holds
    the idle→running→stopped state machine; a second `start()` / later `run()`
    returns `ErrorKind::conflict`. Threads stay inside `Runtime` (async A3).
  - `desktop::run_chat_session(ChatBridge&, TurnRunner)` — the runtime-side
    session loop: take `next_prompt`, run it through the embedder-supplied
    `TurnRunner` (streaming into the bridge's sink), repeat until the prompt
    queue closes; a turn error (incl. cancellation) ends that turn, not the
    session. `TurnRunner` is a `std::function` alias so `oran-desktop` drives the
    loop without depending on `oran-agent`/`oran-bootstrap`.
  - `ChatBridge` per-turn cancellation: the single `cancellation_signal` became
    an `optional` reset by a new `begin_turn()` (fresh slot per turn, called on
    the runtime executor); `request_stop()` now *posts* the terminal emit onto
    the runtime executor so a UI-thread caller never touches the signal
    cross-thread.
  - **Bug fix (root cause):** `ChatBridge::close()` closed *both* the prompt
    (input) and update (output) channels. The session loop ends by closing input
    so `next_prompt` drains-then-fails — but closing `updates_` made every
    streamed delta hit a closed channel (`try_send` rejects → dropped), so
    `drain()` returned 0. `close()` is now **input-only**; the update channel
    stays open so a final in-flight turn's deltas remain drainable, and is freed
    on bridge destruction. A graceful shutdown pairs `close()` with
    `request_stop()`. No committed Slice-C test or the bench relied on the old
    both-channels behavior.
  - Binary version bumped to `2.0.0-slice251`.

### Design Intent

Slice D's plan splits cleanly into an always-built, fully unit-testable runtime
core and a gated (`--desktop=y`) Slint shell. This commit lands the core so every
commit stays CI-verifiable; the shell (manual-smoke-only here) follows as the
next slice. `Runtime::start()` is the rules-compliant way to coexist with Slint
(which owns the main thread): threads remain inside `Runtime` (A3), and detached
coroutines catch their own errors (A8) since `start()` does not surface worker
exceptions. The input-only `close()` matches the real lifecycle — closing input
is how the session loop winds down, and it must not discard the final turn's
output. See `docs/design-docs/async-model.md` (Runtime surface) and
`docs/DESKTOP.md` (bridge seam).

### Files Modified

- `include/oran/async/runtime.hpp` — `Runtime::start()` declaration + contract.
- `src/oran-async/runtime.cpp` — `start()` + shared `spawn_workers()` (refactor of `run()`).
- `include/oran/desktop/chat_bridge.hpp` — `begin_turn()`, `TurnRunner`,
  `run_chat_session`, per-turn `optional<cancellation_signal>`, `request_stop()`
  posts, input-only `close()` contract.
- `src/oran-desktop/chat_bridge.cpp` — `begin_turn`, `request_stop` posts,
  `run_chat_session` loop, `close()` closes input only.
- `tests/async/test_async.cpp` — two `Runtime::start()` cases (non-blocking
  launch off the calling thread; rejects a second start / later run).
- `tests/desktop/test_chat_bridge.cpp` — two `[session]` cases (each prompt runs
  through the turn runner end-to-end; stop cancels the in-flight turn).
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` → `2.0.0-slice251`.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/async-model.md` — `Runtime::start()` in the Public Surface +
  a prose paragraph (shared spawn path, conflict state machine, A8 caveat).
- `docs/ARCHITECTURE.md` — `oran-async` row (`start()`); `oran-desktop` row
  (slice 251 session driver + `Runtime::start()` + input-only `close()`).
- `docs/DESKTOP.md` — Current State: runtime-side glue landed; gated UI = Slice D.
- `docs/STATUS.md` — slice 251, last-history pointer, active-plan line, frontier
  (core landed / next = gated shell), `oran-async` 16/83 + `oran-desktop` 17/70.
- `docs/ROADMAP.md` — Desktop row: slice 251 core prep; next = gated Slint shell.
- `docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md` — progress log +
  Linked Artifacts history line.

### Validation

- Commands run:
  - Diagnosed RED: `test-desktop "[session]"` → 1 case failing
    (`drain(vm)==4` got `0`); root-caused to `close()` closing `updates_`.
  - GREEN after fix: `test-desktop "[session]"` 2 cases / 11 assertions pass;
    full `test-desktop` **17 cases / 70 assertions** (was 15 / 59).
  - `test-async` **16 cases / 83 assertions** (was 14 / 76);
    `[runtime]` 5 cases / 17 assertions.
  - Full default build (`--desktop=n`) clean; `xmake test` → **19/19 buckets pass**.
- Tests added/changed: 2 async `Runtime::start()` cases, 2 desktop `[session]`
  cases (all test-first; the failing session case was the existing RED this slice
  turned GREEN at root cause).
- Bench impact: none (no new workload; `bench-desktop` unchanged).
- Compile-budget delta: negligible — `oran-async` gains one method; `oran-desktop`
  gains a `std::function`-typed free function and asio co_spawn includes already
  present in the bridge TU. No new library deps.

### Follow-ups

- Next: Slice D completion — gated Slint chat UI (input, live transcript, stop)
  bound to the `ChatBridge`; `orangutan --desktop` builds config→runtime+provider
  (scripted `FakeProvider` fallback when no route resolves) + the `TurnRunner`
  adapting `AgentPromptRunner`, calls `Runtime::start()`, co-spawns
  `run_chat_session`, runs the shell; spec 0007 acceptance 1–3; manual
  `--desktop=y` run; release note.
- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: deferred to Slice D (when `orangutan --desktop` becomes
  user-visible).

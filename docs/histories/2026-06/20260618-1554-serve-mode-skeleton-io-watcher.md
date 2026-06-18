## [2026-06-18 15:54] | Task: runtime-service owner Slice A — `--serve` skeleton + IO file-view watcher auto-start

### Execution Context

- Agent: `claude-code`
- Base model: `claude-opus-4-8[1m]`
- Runtime: Claude Code CLI, GCC 16.1 / xmake, WSL2.
- Linked plan: `docs/exec-plans/active/2026-06-18-runtime-service-owner.md` (Slice A)

### User Query

> Continue iterating and implementing the project's other modules; find the
> highest-priority part to implement. Skip the desktop module for now. (The
> chosen track was Dependency Frontier #2 — the runtime-service / daemon owner.)

### Changes Overview

- Areas: `oran-bootstrap` (new long-lived `--serve` mode).
- Key actions:
  - New `bootstrap::run_serve` + `serve_run` — a service mode of the main binary,
    parallel to `--desktop`. `run_serve` loads config, builds `async::Runtime`,
    traps SIGINT/SIGTERM on a runtime **strand**, co-spawns `serve_run` bound to an
    `asio::cancellation_signal`, blocks on a completion `promise` until a signal,
    then gracefully cancels, `runtime.stop()`s, and returns a `cancelled` error
    carrying `signal`/`signum` (the shared seam `run` maps to `128 + signum`).
  - `serve_run` runs the IO file-view cache watcher
    (`io::watch_read_text_file_ranged_cache`, `max_events = 0` = run-until-cancelled)
    and idles until its bound cancellation slot fires, returning `Result<void>{}`
    (a graceful stop is not an error). A watcher that cannot initialize (e.g.
    inotify unavailable, missing root) is **non-fatal**: reported once, then the
    service keeps idling until signalled.
  - `bootstrap.cpp` — `ParsedArgs::serve`, `--serve` parsing beside `--desktop`,
    a `run()` dispatch branch with the `--audit-init`/`--trace` signal→exit-code
    translation, usage text, and `kVersion` → `2.0.0-slice253`.

### Design Intent

Resolves the IO-watcher-auto-start leg of ROADMAP Dependency Frontier #2 and lays
the lifecycle the automation loop (slice B) and scheduler reaping tick (slice C)
will plug into. `--serve` is a mode of the main binary (not a separate
`orangutan-server`) per `ARCHITECTURE.md`'s single-binary / N-runtimes model and the
`--desktop` precedent. The lifecycle mirrors `run_desktop`'s start → co-spawn →
block → graceful-teardown shape, but blocks on a signal instead of a UI loop and
stops the service coroutine via a fine-grained `asio::cancellation_signal` rather
than `SignalScope`/`io.stop()` — deliberately, so later concerns' SQLite writes and
in-flight agent turns are never blunt-dropped (the evolution `signal_drain.hpp`
already anticipates). The signal handler and `serve_run` share one strand so the
cross-thread `cancellation_signal::emit` is serialized with slot consumption,
regardless of io-worker count. Slice A intentionally does **not** build
`RuntimeAssembly` (the watcher needs only the executor + root); slice B adds it when
automation needs audit/workspace.

### Files Modified

- `include/oran/bootstrap/serve.hpp` — new: `ServeOptions`, `serve_run`, `run_serve`.
- `src/oran-bootstrap/serve.cpp` — new: lifecycle + watcher concern + idle helper.
- `include/oran/bootstrap.hpp` — umbrella registers `serve.hpp`.
- `src/oran-bootstrap/bootstrap.cpp` — `--serve` flag/dispatch/usage; `kVersion`
  → `2.0.0-slice253`; include `<oran/bootstrap/serve.hpp>`.
- `tests/bootstrap/test_serve.cpp` — new: 4 `[serve]` cases.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/bootstrap-runtime.md` — new "Service Mode (`--serve`)" section;
  Next Steps refreshed.
- `docs/design-docs/io-runtime.md` — watcher auto-start now wired by `--serve`.
- `docs/ARCHITECTURE.md` — `oran-bootstrap` row: `--serve` / `run_serve` lifecycle.
- `docs/STATUS.md` — slice 253, last-history pointer, frontier, active exec-plans,
  oran-bootstrap surface count.
- `docs/ROADMAP.md` — File-view & IO row (watcher auto-start owned by `--serve`);
  Dependency Frontier #2 marked partially resolved; CLI row notes `--serve`.
- `docs/exec-plans/active/2026-06-18-runtime-service-owner.md` — the A→D plan.
- `docs/releases/feature-release-notes.md` — `orangutan --serve` row.

### Validation

- Commands run: `xmake f -m release && xmake build test-bootstrap` (clean);
  `test-bootstrap "[serve]"` → 4/4 (9 assertions); full `test-bootstrap` → **161
  cases / 1590 assertions** (+4 / +9 vs slice 252). Debug `--sanitizers=y`: `[serve]`
  and the full bootstrap suite pass clean (no UB/leaks). Smoke:
  `orangutan --serve` prints the banner, idles, exits **130** on SIGINT and **143**
  on SIGTERM.
- Tests added/changed: `tests/bootstrap/test_serve.cpp` (idle-until-cancel with a
  timing lower bound; empty-root disables watcher; watcher-runs-then-cancels;
  watcher-init-failure is non-fatal).
- Bench impact: none (lifecycle only).
- Compile-budget delta: one small TU + header added to `oran-bootstrap`; no new deps.

### Follow-ups

- Slice B: automation cron/triggered loop under `--serve` (open `automation.db`,
  config cron seeds, prompt-backed handler over a configured route,
  `AutomationService::run` with a signal-tied stop predicate).
- Slice C: scheduler idle-lock reaping tick (first hoist `ToolScheduler` ownership
  out of `AgentPromptRunner`).
- Tech-debt entries: none.
- Linked release note: `orangutan --serve` (feature-release-notes.md).

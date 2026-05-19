## [2026-05-19 23:50] | Task: signal-aware shutdown for `bootstrap::run`'s io_context drain (slice 23)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — single-session slice that fits the
  `Next intended slice` bullet in [`STATUS.md`](../../STATUS.md)
  ("signal-aware shutdown for `bootstrap::run` so SIGINT terminates the
  io_context drain promptly"), matching `PLANS_GUIDE.md` "When NOT To
  Create A Plan". The two other deferred follow-ups (blocking hook
  semantics; `tool_dispatched` / `tool_error` events) were explicitly
  parked by slice 22 because their first consumer lives in the
  not-yet-existing `oran-agent` library; the Anthropic adapter is
  multi-slice and needs an exec plan ahead of code. Signal-aware
  shutdown is the only candidate that ships user-visible value today
  without a downstream consumer waiting on `oran-agent`.

### User Query

> 深度了解项目，查看当前项目真实进度, 继续推进项目代码的实现. ultrathink.
>
> (Understand the project deeply, check the real current progress,
> continue advancing the project code implementation. Ultrathink.)

### Changes Overview

- **New `oran-bootstrap` public surface** — `include/oran/bootstrap/signal_drain.hpp`:
  - `bootstrap::SignalScope` — RAII trap that installs `asio::signal_set`
    handlers for SIGINT + SIGTERM on a given `asio::io_context`. The
    scope is non-copyable / non-movable; constructing it issues a single
    `async_wait` whose handler stores the POSIX signum and calls
    `io.stop()` on first delivery. `release()` cancels the pending
    `async_wait` (idempotent) so `io.run()` can return naturally once
    every *other* posted item has drained. `signum()` reads the captured
    integer (`0` until a signal has fired).
  - `bootstrap::signal_name(int)` — stable spelling for the captured
    signum (`"SIGINT"`, `"SIGTERM"`, `"unknown"`), so error context
    entries stay stable across log/redaction passes.
  - `bootstrap::signum_from_error(const core::Error&)` — `std::optional<int>`
    accessor that pulls the signum out of a signal-driven `cancelled`
    error. Returns `nullopt` for non-cancelled kinds, when the
    `signum` context entry is missing, or when it fails to parse as a
    positive integer. The helper is what lets `bootstrap::run`
    translate signal-driven cancellation into shell-conventional exit
    codes (128 + signum) without poking at `Error::context()` directly.
- **`bootstrap::run_audit_init` adopts the scope.** The one-shot
  `asio::io_context` that drives the audit migration now constructs a
  `SignalScope` before the `Pool::open` + `co_spawn` calls. The
  migration coroutine calls `signals.release()` at its tail so
  `io.run()` returns naturally on success; on SIGINT/SIGTERM the
  `io.stop()` callback fires first and `io.run()` returns with
  `scope.signum() != 0`. The function translates the signal case into
  `Error::cancelled().with("signal", signal_name(sig)).with("signum",
  to_string(sig))`.
- **`bootstrap::run` translates the signal into a shell exit code.**
  When `run_audit_init` returns `cancelled` with a recoverable signum,
  `run` prints `orangutan: interrupted by <name> (<num>)` to stderr
  and returns `Result<int>(128 + signum)`. Non-signal cancelled errors
  propagate verbatim.
- **Slice-version bump.** `kVersion` 22 → 23. `xmake run orangutan --help`
  reports `orangutan v2.0.0-slice23`.
- **Tests.** `tests/bootstrap/test_signal_drain.cpp` ships ten cases
  covering: empty drain returns success when `release()` runs before
  `io.run()`; pending work runs to completion when `release()` is
  posted after; in-process `raise(SIGTERM)` from a queued post fires
  the scope and stops `io.run()`; same for `raise(SIGINT)`; idempotent
  `release()`; `signum_from_error` recovers the integer; rejects
  non-cancelled errors; rejects missing context; rejects malformed
  context (`abc`, `-1`, `15x`); `signal_name` maps the supported
  signals. `test-bootstrap` grows to 44 cases / 140 assertions (+10
  cases / +17 assertions). No existing test required modification —
  the only callsite of `--audit-init`'s drain path stays
  behaviourally identical for the success case.
- **Bench.** New `bench/bootstrap/scenarios/signal_drain.cpp` ships an
  A/B `signal_drain_with_scope` (~11.2 µs) vs. `signal_drain_bare`
  (~443 ns) over the same 8-post workload — the ~10.7 µs delta is the
  per-drain cost of installing the `asio::signal_set` and cancelling
  its `async_wait` afterward. Small relative to `--audit-init`'s
  ~202 µs migration cost.

### Design Intent

**Why a RAII scope + explicit `release()` instead of a "drain"
function.** The first design attempt was a `drain_with_signals(io)`
helper that owned the `signal_set` and called `io.run()` internally.
That design didn't work — `asio::signal_set::async_wait` is itself
pending work on the executor, so `io.run()` blocks forever waiting for
a signal that may never arrive. The fix needed by every realistic
caller is "cancel the signal_set's wait once your real work has
completed so the io_context can drain naturally." The RAII scope shape
exposes that release point as an explicit `release()` call, which is
trivial to wire into a migration coroutine's tail and stays out of the
way when the caller wants different lifetime semantics. The signum is
captured atomically so the post-drain inspection from another thread
(the test harness, eventually the agent loop's main thread) is safe.

**Why `io.stop()` rather than `asio::cancellation_signal`.** The
slice's only caller is `run_audit_init`'s migration coroutine, which
is a one-shot SQLite write the WAL journal commits atomically. A
blunt `io.stop()` drop is safe here — re-running `--audit-init`
either sees the pre-migration or post-migration state, never a
half-state. The agent loop will need fine-grained cancellation later
(per `docs/rules/critical-rules.md#C11`), but wiring an
`asio::cancellation_signal` end-to-end requires the loop's
cancel-state plumbing which doesn't exist yet. The header docstring
calls this out so the future slice that introduces the agent loop
knows to revisit the choice.

**Why SIGINT + SIGTERM, not also SIGHUP / SIGQUIT.** SIGINT covers
Ctrl-C, SIGTERM covers the default `kill` and most service managers
(systemd, runit, supervisord). SIGHUP is the standard "reload config"
signal — folding it into the same shutdown path would prevent the
future "reload on SIGHUP" feature the channels/web layer will want.
SIGQUIT defaults to core dump and is a debugging signal; trapping it
loses that. The signal set can grow when a feature needs each one;
keeping it narrow now leaves the design room to differentiate.

**Why translate cancellation in `bootstrap::run` rather than
`main.cpp`.** `main.cpp` is the kernel-of-truth-translation seam (exit
code per `Result<int>`); pushing the signal-specific logic up to it
would force `main.cpp` to know about `signum_from_error`,
`signal_name`, and the 128+signum convention. Keeping the translation
in `bootstrap::run` lets `main.cpp` stay a four-line dispatcher and
lets future bootstrap subcommands (web server, channel adapter) opt
into the same translation without duplicating the logic. The
canonical "interrupted by SIGINT (2)" stderr line is printed from
`bootstrap::run` for the same reason.

**Why `signum_from_error` parses from `Error::context()` rather than
exposing a typed accessor on `Error`.** `Error` is the cross-library
boundary type — adding a typed `signum()` accessor would pollute every
non-signal error site with the field. The existing `with(key, value)`
context API is exactly what's there for; the helper just gives
callers a typed view onto a documented key. Future similar accessors
(`retry_after_ms`, `decision_reason`, …) follow the same pattern —
they exist already in `oran-permission` and `oran-tool`.

**Why a SignalScope test deliberately raises SIGTERM/SIGINT
in-process.** Catch2 doesn't install its own signal handlers by
default, so the test process inherits the default disposition
(terminate). As soon as `SignalScope` constructs, `asio::signal_set`
installs handlers via `sigaction` that catch SIGINT/SIGTERM and post
them to the io_context. `raise(SIGTERM)` from a queued post therefore
delivers the signal to the same thread, asio's handler fires, the
async_wait callback runs, `io.stop()` returns, and `io.run()` exits.
The scope's destructor restores the previous disposition (default
terminate) when it leaves the test case — subsequent tests in the
bucket are unaffected. This is the testing pattern asio's own
internal tests use; it's the cheapest deterministic way to exercise
the cancellation path without a subprocess.

### Files Modified

- `include/oran/bootstrap/signal_drain.hpp` — new, 86 lines:
  `SignalScope` + `signal_name` + `signum_from_error` public surface.
- `src/oran-bootstrap/signal_drain.cpp` — new, 77 lines: `Impl`
  pimpl with `asio::signal_set` + atomic signum; `release()`
  cancellation; `signal_name` switch; `signum_from_error` parser via
  `std::from_chars`.
- `include/oran/bootstrap.hpp` — umbrella header now includes
  `<oran/bootstrap/signal_drain.hpp>`.
- `src/oran-bootstrap/bootstrap.cpp` — `run_audit_init` builds a
  `SignalScope`, releases it at the tail of the migration coroutine,
  and translates `scope.signum() != 0` into `Error::cancelled`.
  `bootstrap::run` translates that error into 128+signum and prints
  the interrupted line to stderr. `kVersion` 22 → 23.
- `tests/bootstrap/test_signal_drain.cpp` — new, 10 cases / 17
  assertions.
- `bench/bootstrap/scenarios/signal_drain.cpp` — new, 60 lines: A/B
  `signal_drain_with_scope` vs. `signal_drain_bare`.
- `bench/bootstrap/main.cpp` — registers `register_signal_drain`
  between `register_config_startup` and `register_runtime_assembly_build`.
- `bench/bootstrap/README.md` — documents the new A/B.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 23, history pointer, library surfaces row
  for `oran-bootstrap` (44 cases / 140 assertions), refreshed
  `Next intended slice` to drop the now-landed signal-aware shutdown.
- `docs/QUALITY_SCORE.md` — Bootstrap row rewritten to describe the
  new `SignalScope` surface + `--audit-init` integration + exit-code
  translation; Test framework row refreshed with `oran-bootstrap`
  44/140; Bench harness row extended with the new
  `signal_drain_with_scope` / `signal_drain_bare` A/B.
- `docs/ARCHITECTURE.md` — slice-status preamble now lists slice 23
  alongside the prior slices; `oran-bootstrap` inventory row updated
  to mention `SignalScope` + exit-code translation.
- `docs/design-docs/async-model.md` — Cancellation section notes that
  `bootstrap::SignalScope` is the first concrete consumer of the
  cancel-aware-drain contract.
- `docs/releases/feature-release-notes.md` — new top row
  `bootstrap-signal-aware-shutdown`.
- `docs/histories/2026-05/20260519-2350-bootstrap-signal-aware-shutdown.md` —
  this file.

### Validation

- Commands run:
  - `xmake build oran-bootstrap` — clean (~7 s).
  - `xmake build test-bootstrap` — clean (~10 s).
  - `./build/linux/x86_64/release/test-bootstrap [signal_drain]` —
    10 cases / 17 assertions, all green.
  - `./build/linux/x86_64/release/test-bootstrap` — 44 cases / 140
    assertions, all green.
  - `xmake test` — all 10 buckets green (test-async / bootstrap /
    cli / config / core / hook / io / permission / storage / tool).
  - `xmake build bench-bootstrap && xmake run bench-bootstrap` —
    clean; measured `signal_drain_with_scope ~11,197 ns` vs.
    `signal_drain_bare ~443 ns`, `assembly_build_with_audit ~204,931 ns`
    unchanged; `bootstrap.config_missing_default ~1,331 ns` and
    `config_explicit_file ~7,312 ns` unchanged within noise.
  - `xmake build orangutan && xmake run orangutan -- --help` —
    prints the slice-23 banner; the CLI surface is unchanged.
  - `./build/linux/x86_64/release/orangutan --audit-init /tmp/x.db` —
    `audit schema ready: version 1 at /tmp/x.db (1 migrations applied)`,
    exit 0. Interactive Ctrl-C path verified by inspection only —
    the `raise()` tests pin the asio handler behaviour and the exit
    code translation is mechanical.
- Tests added/changed: 10 new bootstrap-bucket cases (+17
  assertions); no existing test required modification.
- Bench impact: existing scenarios unchanged within noise. New scenarios
  baselined above.
- Compile-budget delta: one new TU in `oran-bootstrap` (`signal_drain.cpp`,
  ~77 lines, only `<asio/signal_set.hpp>` + `<oran/core/error.hpp>`);
  one new TU in `bench-bootstrap` (`signal_drain.cpp`, ~60 lines). Both
  consume only PCH + asio + the bootstrap library, so build-time impact
  is in the same envelope as the existing scenarios.

### Follow-ups

- Issues to file: none.
- Tech-debt entries filed: none. The agent-loop slice will revisit the
  blunt `io.stop()` choice in favour of `asio::cancellation_signal`
  once the loop's cancel-state plumbing lands — that is already
  captured in the `SignalScope` header docstring.
- Linked release note: 2026-05-19
  `bootstrap-signal-aware-shutdown` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: when the agent loop lands, the
  natural integration is to construct a single `SignalScope` in the
  outer `bootstrap::run` (rather than per-subcommand), wire its
  cancellation into the loop's `asio::cancellation_signal`, and have
  the loop call `scope.release()` at clean-shutdown time. The
  exit-code translation in `bootstrap::run` already handles the cases
  the agent loop will need.

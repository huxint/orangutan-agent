# Runtime-Service / Daemon Owner (`--serve`)

## Goal

Give the long-lived periodic concerns that `oran-automation`, `oran-io`, and the
`agent::ToolScheduler` were deliberately built to expect — but which nothing drives —
a single owner: a `--serve` mode of the main binary (parallel to `--desktop`). When
complete, `orangutan --serve` starts `async::Runtime`, hosts the automation
cron/triggered loop, the IO file-view cache watcher, and the scheduler idle-lock
reaping tick, and shuts every one of them down gracefully on SIGINT/SIGTERM. This
resolves ROADMAP Dependency Frontier #2.

## Scope

- In scope: a bootstrap-owned long-lived service entry (`bootstrap::run_serve` +
  `serve_run`); graceful signal-driven lifecycle on `async::Runtime` +
  `asio::cancellation_signal`; auto-starting the IO file-view watcher; later, the
  automation service loop and the scheduler reaping tick under the same lifecycle.
- Out of scope: a separate `orangutan-server` binary (rejected — the architecture is a
  single binary hosting N runtimes behind M interfaces); a detached/forking daemon;
  multi-tenant scheduling policy; remote control surfaces.

## Context

- Relevant docs: `docs/design-docs/bootstrap-runtime.md` (Service Mode),
  `docs/design-docs/automation-runtime.md`, `docs/design-docs/io-runtime.md`,
  `docs/design-docs/async-model.md`, `docs/ROADMAP.md` (Dependency Frontier #2).
- Relevant code paths: `src/oran-bootstrap/serve.cpp`,
  `include/oran/bootstrap/serve.hpp`, `src/oran-bootstrap/bootstrap.cpp` (`run`
  dispatch, `run_desktop` template), `include/oran/async/runtime.hpp`
  (`start`/`stop`/`make_strand`), `io::watch_read_text_file_ranged_cache`
  (`include/oran/io/file.hpp`), `automation::AutomationService::run`
  (`include/oran/automation/runtime.hpp`), `bootstrap::make_automation_agent_prompt_runner`,
  `agent::ToolScheduler::reap_idle_locks` (`include/oran/agent/scheduler.hpp`).
- Constraints: CI stays secret-free / network-free; C11 (every await cancel-aware);
  C6 (no heavy includes in the public header).
- Compile-budget impact: one new small TU (`serve.cpp`) + header in `oran-bootstrap`;
  no new third-party dependency.

## Risks

- Risk: cross-thread `asio::cancellation_signal` emit under multiple io-workers.
  Mitigation: construct the `signal_set` and co-spawn `serve_run` on one runtime
  strand so the handler's `emit` is serialized with slot consumption; validated under
  `--sanitizers=y`.
- Risk: `runtime.stop()` racing an in-flight service coroutine (use-after-free of
  borrowed services in later slices). Mitigation: block on a completion `promise`
  before `stop()` — the same invariant `run_desktop` uses.
- Risk: blunt-dropping SQLite writes / in-flight agent turns on shutdown (slice B+).
  Mitigation: build the lifecycle on graceful `cancellation_signal` from slice A
  rather than `SignalScope`/`io.stop()`, so later concerns cancel cooperatively.

## Milestones

1. **Slice A — skeleton + IO watcher (shipped, slice 253).** `--serve` flag +
   `run_serve` lifecycle (start → co-spawn `serve_run` → block on signal → graceful
   cancel → stop → `128+signum`) and the IO file-view watcher auto-start. No
   provider/DB; CI-safe.
2. **Slice B — automation loop (shipped, slice 254).** Open
   `<workspace>/.orangutan/automation.db` (`AutomationRuntime::open`), map config cron
   seeds (`cron_jobs_from`) and apply them once, build a prompt-backed handler
   (`make_automation_agent_prompt_runner` + `make_cron_prompt_handler` /
   `make_triggered_prompt_handler`) over a configured `default` route — or an offline
   scripted `FakeProvider` when none resolves — and drive `automation::AutomationService::run`
   as a new `serve_automation` concern (a cancel-aware poll loop), raced beside the
   watcher by a file-local `serve_body`, with a signal-tied `stop_requested` predicate.
3. **Slice C — scheduler idle-lock reaping (shipped, slice 255).** Resolved the
   scheduler-ownership gap: `AgentPromptRunnerOptions` gained an optional
   `{registry, scheduler}` pair (both-or-neither) the runner borrows instead of
   building its own. `run_serve` now builds one strand-hosted `tool::Registry` +
   `agent::ToolScheduler`, injects them into every per-job automation runner, and
   races a new `serve_scheduler_reaping` concern (a cancel-aware `reap_idle_locks`
   poll loop) beside the watcher and automation loop.
4. **Slice D (optional) — typed `serve` config block.** Toggles/intervals for the
   concerns once more than one needs tuning (the reaping interval is a fixed
   1-minute default today). Deferred — not warranted yet.

## Validation

- Commands: `xmake build oran-bootstrap`; `xmake run test-bootstrap "[serve]"`;
  `xmake test`; debug `--sanitizers=y` over `[serve]`; `make ci`.
- Manual checks: `orangutan --serve` in a temp workspace prints the banner, idles,
  and exits `130`/`143` on SIGINT/SIGTERM.
- Observability checks: (slice B+) automation run rows; watcher invalidation stats.
- Bench comparison: n/a for slice A (lifecycle only).

## Progress Log

- [x] 2026-06-18: Slice A shipped — `--serve` + `serve_run` + IO watcher auto-start;
  `test-bootstrap` 161/1590 (+4 cases / +9 assertions); release build + `[serve]` +
  full-suite-under-sanitizers green; SIGINT→130 / SIGTERM→143 smoke verified; docs
  synced (this PR).
- [x] 2026-06-18: Slice B shipped — automation cron/triggered loop under `--serve`.
  New `serve_automation` concern over `AutomationService::run`; `run_serve` opens
  `automation.db`, applies config cron seeds once, builds a prompt-backed handler over
  a live-or-offline-fake provider, and races it beside the watcher via `serve_body`.
  `test-bootstrap` 164/1603 (+3 cases / +13 assertions); release `[serve]` 7/7,
  full suite 19/19, bootstrap clean under `--sanitizers=y`; offline smoke records a
  cron success row and exits 130/143. Docs synced (this PR).
- [x] Slice C: scheduler idle-lock reaping tick (slice 255). Hoisted the
  registry+scheduler ownership out of `AgentPromptRunner` (optional both-or-neither
  injection), built one strand-hosted shared scheduler in `run_serve`, and raced a
  `serve_scheduler_reaping` concern beside the watcher + automation loop.
  `test-bootstrap` 169/1632 (+5 cases / +29 assertions vs slice 254); release `[serve]`
  10/10, `[prompt_runner]` 37/37, full suite 19/19, clean under `--sanitizers=y`;
  offline smoke shows the reaping banner line and exits 130/143. Docs synced (this PR).
- [ ] Slice D: typed `serve` config block — deferred (optional; not warranted yet).

## Decision Log

- 2026-06-18: `--serve` is a mode of the main binary, not a separate
  `orangutan-server`. Rationale: `ARCHITECTURE.md`'s single-binary / N-runtimes model
  and the `--desktop` precedent. Consequence: `run_serve` lives in `oran-bootstrap`
  beside `run_desktop`; no new target.
- 2026-06-18: Slice A builds on `async::Runtime` + a graceful `asio::cancellation_signal`
  rather than the one-shot `SignalScope`/`io.stop()` drain. Rationale: later concerns
  (SQLite writes, agent turns) must not be blunt-dropped; `signal_drain.hpp` itself
  anticipates this evolution. Consequence: `SignalScope` stays the one-shot-command
  drain; `--serve` owns its own strand-serialized signal handling.
- 2026-06-18: Slice A does **not** build `RuntimeAssembly` (a deviation from the
  approved plan's step 3). Rationale: the watcher needs only the executor + root, and
  opening `audit.db` purely to discard the assembly is an unnecessary side effect for
  a file-watching skeleton. Consequence: slice B introduces the assembly when
  automation actually needs audit/workspace/trace.
- 2026-06-18 (Slice B): automation is **gated on config `automation.cron.jobs[]`**.
  With none, `--serve` builds no provider/assembly and opens no `automation.db` — it
  is byte-for-byte the slice-A watcher, keeping the default `--serve` and CI
  secret-free / DB-free. Consequence: a triggered-only deployment needs at least one
  cron seed (or a future explicit toggle) to start the automation concern; acceptable
  until a triggered ingress exists.
- 2026-06-18 (Slice B): `serve_automation` is a **separate exported concern**, not
  folded into `serve_run`. Rationale: independent unit-testability (a real
  `AutomationRuntime` on a temp DB + fake handlers, no provider/process/signal) and a
  clean composition seam (`serve_body`) for Slice C. Consequence: `serve_run` keeps
  its slice-A signature and tests unchanged.
- 2026-06-18 (Slice B): the **`stop_requested` predicate is the authoritative stop**,
  not parent cancellation. `oran-automation`'s service disables cancellation around
  durable lease/run-row writes, so a firing tick can swallow a parent cancellation;
  the predicate (tied to the trapped signum, checked before and after each tick)
  guarantees prompt termination, and `run_serve` always supplies it. Discovered when a
  pure `||`-cancel-after-fire unit test hung; resolved by the post-tick predicate
  check. Consequence: any future long-lived caller of `serve_automation` must supply
  the predicate.
- 2026-06-18 (Slice B): an **offline scripted `FakeProvider`** fallback (mirroring
  `--desktop`) drives the loop when no `default` route resolves. Rationale: keep
  `--serve` automation demonstrable without credentials and CI secret-free.
  Consequence: an offline deployment fires scripted replies; a real route swaps in
  through the identical `make_automation_agent_prompt_runner` bridge.
- 2026-06-20 (Slice C): the **scheduler is hoisted via optional both-or-neither
  `{registry, scheduler}` injection** on `AgentPromptRunnerOptions`, not a mandatory
  parameter. Rationale: the CLI / channel / desktop callers keep the self-owned path
  unchanged (zero behavior change); only `--serve` needs a shared scheduler. The pair
  is all-or-nothing because the runner's `.tools` and the scheduler's internal
  registry must be the *same* `tool::Registry` instance. Consequence: `create`
  rejects exactly one of the two.
- 2026-06-20 (Slice C): the shared scheduler and the per-job automation runners run
  on the **runtime strand**, not the multi-worker `runtime.executor()`. Rationale:
  the per-path lock table is single-strand by contract
  (`src/oran-agent/_impl/path_lock_table.hpp`), so `run_batch`'s lock acquire/release,
  its `async::Channel`s, and the reaping `reap_idle_locks` must all be serialized on
  one executor. Consequence: this also *corrects* the slice-B per-job scheduler, which
  ran on the 4-worker executor and could race the lock table on a multi-tool batch;
  automation tool dispatch is now single-threaded coordination (IO still offloads to
  oran-io's pool, so effective parallelism is preserved).
- 2026-06-20 (Slice C): reaping deliberately **drops its reap count and never disables
  cancellation**. Rationale: it is a synchronous in-memory sweep, so unlike the
  automation tick it cannot swallow a parent cancellation; pre-`oran-log` there is no
  structured sink, and `lock_stats().reaped_entries` already exposes the cumulative
  total. Consequence: the `stop_requested` predicate is honored for symmetry but the
  idle-wait cancel is the primary stop.
- 2026-06-20 (Slice C): the CLI / channel agent loops still drive their scheduler on
  the multi-worker `runtime.executor()` (this slice fixed only the `--serve`
  automation path). Logged as tech-debt (single-strand lock-table contract) rather
  than fixed here to keep the slice scoped.

## Linked Artifacts

- Related design doc: `docs/design-docs/bootstrap-runtime.md` (Service Mode),
  `docs/design-docs/automation-runtime.md`, `docs/design-docs/io-runtime.md`.
- Related product spec: `docs/product-specs/0012-tool-scheduler-and-state.md` (AC10
  reaping tick); no dedicated spec for `--serve` (tracked via this plan + ROADMAP).
- PRs: slice A (skeleton + watcher), slice B (automation loop), slice C (this PR —
  scheduler reaping + ownership hoist).
- History entries: `docs/histories/2026-06/20260618-1554-serve-mode-skeleton-io-watcher.md`,
  `docs/histories/2026-06/20260618-1719-serve-automation-cron-loop.md`,
  `docs/histories/2026-06/20260620-1059-serve-scheduler-idle-lock-reaping.md`.
- Release note: `docs/releases/feature-release-notes.md` (`orangutan --serve`).

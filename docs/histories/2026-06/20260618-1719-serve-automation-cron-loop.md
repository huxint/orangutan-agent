## [2026-06-18 17:19] | Task: runtime-service owner Slice B — automation cron/triggered loop under `--serve`

### Execution Context

- Agent: `claude-code`
- Base model: `claude-opus-4-8[1m]`
- Runtime: Claude Code CLI, GCC 16.1 / xmake, WSL2.
- Linked plan: `docs/exec-plans/active/2026-06-18-runtime-service-owner.md` (Slice B)

### User Query

> Continue iterating and implementing the project's other modules; find the
> highest-priority part. Skip desktop. ("continue") — the chosen next leg was
> Slice B of the runtime-service-owner plan (ROADMAP Dependency Frontier #2).

### Changes Overview

- Areas: `oran-bootstrap` (`--serve` automation concern + provider/DB wiring).
- Key actions:
  - New exported `bootstrap::serve_automation(executor, AutomationService&,
    cron_handler, triggered_handler, ServeAutomationOptions, stop_requested)` — a
    cancel-aware poll loop that drives `automation::AutomationService::run` once per
    tick (`now = core::time::now_utc()`, `max_total_wait = 0`, `max_iterations = 1`),
    firing any cron job due at the current UTC minute and draining buffered triggered
    work, then sleeping `poll_interval`. Cron seeds are applied *once* before the loop
    (not per tick) so stored `last_fired_at` is never reset. A repository error is
    non-fatal (report once → idle until cancelled); handler failures are recorded by
    the service as run rows and do not stop the loop.
  - `run_serve` now gates automation on config `automation.cron.jobs[]`: with none it
    is exactly the slice-A watcher. With cron jobs it builds a `RuntimeAssembly` +
    provider (`HttpProviderBackend` for a configured `default` route, else an offline
    scripted `FakeProvider`), and a file-local `serve_body` coroutine opens
    `<workspace>/.orangutan/automation.db` (`AutomationRuntime::open`), applies seeds
    (`cron_jobs_from` → `apply_cron_job_seeds`), builds a prompt-backed handler
    (`make_automation_agent_prompt_runner` → `make_cron_prompt_handler` /
    `make_triggered_prompt_handler`), and races `serve_automation` beside `serve_run`
    (the watcher) under one cancellation slot via awaitable-operators `||`.
  - `serve.hpp` grows `ServeAutomationOptions` + the `serve_automation` declaration
    (includes `<oran/automation.hpp>`, the same precedent as
    `automation_prompt_runner.hpp`); `bootstrap.cpp` updates `--serve` usage text and
    `kVersion` → `2.0.0-slice254`.

### Design Intent

Resolves the automation-service-loop leg of ROADMAP Dependency Frontier #2 — the
"big payload" the slices 187→225 automation arc was built caller-owned to wait for.
The owner stays a mode of the main binary (the slice-A lifecycle), and the library
stays caller-owned: `--serve` is simply the first long-lived *caller*. `serve_run`
(watcher) and `serve_automation` are kept as independent, separately testable
concerns; `serve_body` composes them so future legs (Slice C scheduler reaping) drop
in the same way. `AutomationService::run` is finite and caller-clocked, so the poll
loop re-supplies `now_utc()` each tick rather than letting one call sleep — this keeps
the triggered-drain responsive and the cron cadence at minute granularity without a
hidden deadline. The offline `FakeProvider` fallback mirrors `--desktop` so the loop
is demonstrable without credentials and CI stays secret-free.

**Cancellation care-point:** `oran-automation`'s service disables cancellation around
its durable lease/run-row writes (`service.cpp` `reset_cancellation_state(
disable_cancellation())`), so a *firing* tick can swallow a parent cancellation that
arrives mid-tick. `serve_automation` therefore treats the `stop_requested` predicate
(tied to the trapped signum) as the authoritative, guaranteed stop — checked before
*and* after each tick — and `run_serve` always supplies it. Idle ticks do not disable
cancellation, so the common idle shutdown stays immediate via the poll-sleep cancel.
Verified empirically: a `||`-cancel after a fire only terminates once the predicate is
supplied; the unit tests and smoke confirm prompt shutdown with it.

### Files Modified

- `include/oran/bootstrap/serve.hpp` — `ServeAutomationOptions`, `serve_automation`
  declaration + doc; `<oran/automation.hpp>` include; header/`run_serve` docs.
- `src/oran-bootstrap/serve.cpp` — `serve_automation` impl, file-local `serve_body`
  composition, offline `serve_offline_plan`/`serve_offline_route`, `run_serve`
  rewrite (provider/assembly/automation.db wiring); new includes.
- `src/oran-bootstrap/bootstrap.cpp` — `--serve` usage text; `kVersion` →
  `2.0.0-slice254`.
- `tests/bootstrap/test_serve.cpp` — 3 new `[serve][automation]` cases (+ shared
  `seed_cron_job` helper, `test::run_async` harness).

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/bootstrap-runtime.md` — Service Mode section: the two concerns,
  `run_serve` automation wiring, the cancellation care-point; Next Steps refreshed.
- `docs/design-docs/automation-runtime.md` — slice-254 note: the owner now drives
  `AutomationService::run` while the library stays caller-owned.
- `docs/ROADMAP.md` — Automation row (Slice B shipped; next = triggered ingress),
  Tool scheduler row (pre-dep), Dependency Frontier #2 (automation leg done).
- `docs/STATUS.md` — slice 254, last-history pointer, active exec-plans, latest/next
  slice, oran-bootstrap surface count (164 / 1603; gated 156 / 1382).
- `docs/exec-plans/active/2026-06-18-runtime-service-owner.md` — Slice B marked done;
  progress log + decision log.
- `docs/releases/feature-release-notes.md` — automation-under-`--serve` row.

### Validation

- Commands run: `xmake f -m release && xmake build oran-bootstrap` (clean);
  `test-bootstrap "[serve]"` → 7/7 (22 assertions); full `test-bootstrap` → **164
  cases / 1603 assertions** (+3 / +13 vs slice 253); `xmake test` → 19/19 buckets.
  Debug `--sanitizers=y`: `[serve]` and the full bootstrap suite pass clean. Offline
  smoke (`--serve` + a routeless config with a due every-minute cron job): records a
  `automation_cron_runs` success row via the `FakeProvider`, exits **143** on SIGTERM
  and **130** on SIGINT.
- Tests added/changed: `tests/bootstrap/test_serve.cpp` — fires a due cron job then
  stops (predicate); idles when not due then stops on `||`-cancel; honors an immediate
  stop predicate before any tick.
- Bench impact: none.
- Compile-budget delta: no new TU/header (extends the slice-A `serve.cpp`/`serve.hpp`);
  `serve.hpp` now pulls `<oran/automation.hpp>`, already in the bootstrap umbrella via
  `automation_cron.hpp` / `automation_prompt_runner.hpp`, so no umbrella regression.

### Follow-ups

- Slice C: scheduler idle-lock reaping tick (first hoist `ToolScheduler` ownership out
  of `AgentPromptRunner`).
- A triggered-work ingress (channel/webhook → `AutomationService::enqueue_triggered`)
  so the triggered half of the `--serve` loop has a producer.
- Tech-debt entries: none.
- Linked release note: automation cron/triggered loop under `--serve`
  (feature-release-notes.md).

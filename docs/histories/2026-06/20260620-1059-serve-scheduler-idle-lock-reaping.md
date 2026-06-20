## [2026-06-20 10:59] | Task: runtime-service owner Slice C — tool-scheduler idle-lock reaping under `--serve`

### Execution Context

- Agent: `claude-code`
- Base model: `claude-opus-4-8[1m]`
- Runtime: Claude Code CLI, GCC 16.1 / xmake, WSL2.
- Linked plan: `docs/exec-plans/completed/2026-06-18-runtime-service-owner.md` (Slice C — final leg; plan now complete)

### User Query

> Continue iterating and implementing the project's other modules; find the
> highest-priority part. ("继续") — the chosen next leg was Slice C of the
> runtime-service-owner plan, the last remaining leg of ROADMAP Dependency
> Frontier #2 and the explicit "Next intended slice" in `STATUS.md`.

### Changes Overview

- Areas: `oran-bootstrap` (`AgentPromptRunner` scheduler-ownership hoist + `--serve`
  scheduler reaping concern); docs.
- Key actions:
  - **Hoisted scheduler ownership out of `AgentPromptRunner`.** `AgentPromptRunnerOptions`
    gains an optional both-or-neither `{registry, scheduler}` pair. When both are set
    the runner borrows them (the `Impl` holds `std::optional<tool::Registry>` /
    `std::optional<agent::ToolScheduler>` for the self-owned case plus `registry_` /
    `scheduler_` pointers that resolve to owned-or-injected); when both are null it
    self-owns exactly as before (CLI / channel / desktop — zero behavior change).
    `create` rejects exactly one of the pair, and only builds its own builtin registry
    on the self-owned path. `AutomationAgentPromptRunnerOptions` forwards the pair to
    every per-job runner. The config→`ToolSchedulerOptions` mapping moved to a shared
    `bootstrap::scheduler_options_from(const config::Config&)` so `create` and `--serve`
    share one source of truth.
  - **New `bootstrap::serve_scheduler_reaping` concern.** A cancel-aware loop that
    sleeps `reap_interval` (default 60 s, `ServeSchedulerReapOptions`) then calls
    `agent::ToolScheduler::reap_idle_locks(core::time::now_utc())`, honoring an optional
    `stop_requested` predicate. Exposed (not file-local) for direct unit testing.
  - **`run_serve` wires it (automation-enabled only).** Builds one `tool::Registry` +
    `agent::ToolScheduler` on its stack, driven on the **runtime strand** (not the
    multi-worker `runtime.executor()`), injects them into the per-job automation runner
    (whose executor is now also the strand), and `serve_body` races
    `serve_scheduler_reaping` beside the watcher and automation loop under one
    cancellation slot. The startup banner gains a reaping-cadence line; `kVersion` →
    `2.0.0-slice255` and the `--serve` usage text now names the reaping tick.

### Design Intent

Resolves the last leg of ROADMAP Dependency Frontier #2. The reaping tick only does
real work against a *long-lived* lock table, but `make_automation_agent_prompt_runner`
builds a fresh `AgentPromptRunner` (hence a fresh scheduler) per job — so its locks die
with each job and there is nothing to reap. That is exactly why "hoist `ToolScheduler`
ownership out of `AgentPromptRunner`" was the stated precondition: `--serve` now owns
*one* scheduler that every per-job runner shares, so locks accumulate into a single
table the reaping concern can bound (spec 0012 AC10).

**Single-strand discipline.** The per-path lock table is single-strand by contract
(`src/oran-agent/_impl/path_lock_table.hpp`): `acquire`/`release`/`reap` must run on
one executor. So the shared scheduler, the per-job runners that dispatch through it,
and the reaping tick all run on the runtime strand. This also *corrects* the slice-B
per-job scheduler, which ran on the default 4-worker `runtime.executor()` — a multi-
tool batch there fans `run_call` (and its lock-table mutations + `async::Channel`s)
across worker threads, a latent data race. Running automation on the strand removes it;
IO still offloads to oran-io's pool, so effective parallelism for IO-bound tools is
preserved. The same hazard remains in the CLI/channel loops and is logged as tech-debt
(they were out of scope for this slice).

**Reaping is cancellation-trivial.** `reap_idle_locks` is a synchronous in-memory sweep
that never awaits or disables cancellation, so — unlike the automation tick — it cannot
swallow a parent cancellation. The `stop_requested` predicate is honored for symmetry,
but the idle-wait cancel is the primary stop. The reap clock is `now_utc()`, matching
the lock table's acquire/release idle stamp.

### Files Modified

- `include/oran/bootstrap/prompt_runner.hpp` — `<oran/agent/scheduler.hpp>` include;
  optional `{registry, scheduler}` fields on `AgentPromptRunnerOptions`;
  `scheduler_options_from` declaration.
- `src/oran-bootstrap/prompt_runner.cpp` — `Impl` borrow-or-own restructure
  (`owned_registry_`/`owned_scheduler_` optionals + `registry_`/`scheduler_` pointers);
  both-or-neither validation; `create` builds the registry only when self-owning;
  `scheduler_options_from` lifted to namespace scope.
- `include/oran/bootstrap/automation_prompt_runner.hpp` — `{registry, scheduler}` fields
  on `AutomationAgentPromptRunnerOptions`.
- `src/oran-bootstrap/automation_prompt_runner.cpp` — forwards the pair in
  `runner_options_for`.
- `include/oran/bootstrap/serve.hpp` — `ServeSchedulerReapOptions`,
  `serve_scheduler_reaping` declaration, `agent::ToolScheduler` forward decl, doc
  updates.
- `src/oran-bootstrap/serve.cpp` — `serve_scheduler_reaping` impl; `serve_body` races
  the third concern; `run_serve` builds + injects the shared strand-driven
  registry/scheduler and passes it to `serve_body`; banner line; new includes.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` → `2.0.0-slice255`; `--serve` usage
  text.
- `tests/bootstrap/test_serve.cpp` — 3 `[serve][scheduler]` cases (reaps across ticks
  then stops on predicate; immediate-stop predicate leaves the entry; graceful cancel)
  + lock-population fixtures.
- `tests/bootstrap/test_prompt_runner.cpp` — 2 `[prompt_runner][scheduler]` cases
  (injected scheduler observes the runner's FileRead lock acquire; create rejects an
  unpaired registry/scheduler).

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/bootstrap-runtime.md` — Service Mode: third concern, the ownership
  hoist, the single-strand rationale; Next Steps refreshed (Slice C removed, Slice D
  deferred, CLI/channel strand follow-up added).
- `docs/product-specs/0012-tool-scheduler-and-state.md` — status note: AC10 background
  tick shipped under `--serve`; `AgentPromptRunner` can borrow an injected pair.
- `docs/ROADMAP.md` — Tool scheduler row (reaping + hoist shipped); Dependency Frontier
  #2 resolved (all three legs shipped; plan moved to completed); plan path refs.
- `docs/STATUS.md` — slice 255, last-history pointer, active exec-plans (drop the
  completed runtime-service-owner plan), latest/next slice, oran-bootstrap surface count.
- `docs/exec-plans/completed/2026-06-18-runtime-service-owner.md` — moved from `active/`;
  Slice C marked shipped; progress + decision logs; linked artifacts.
- `docs/releases/feature-release-notes.md` — slice-255 row.
- `docs/exec-plans/tech-debt-tracker.md` — new row: CLI/channel scheduler runs on the
  multi-worker executor vs the single-strand lock-table contract.

### Validation

- Commands run: `xmake f -m release && xmake build oran-bootstrap` (clean);
  `test-bootstrap "[serve]"` → 10/10, `"[prompt_runner]"` → 37/37; full `test-bootstrap`
  → **169 cases / 1632 assertions** (+5 / +29 vs slice 254); `xmake test` → 19/19
  buckets. Debug `--sanitizers=y`: `[serve]` and `[prompt_runner]` clean. Offline smoke
  (`--serve` + a routeless config with a due every-minute cron job): banner prints
  `scheduler:  idle-lock reaping every 60s`, the offline cron fires (automation.db
  created), and the process exits **130** on SIGINT / **143** on SIGTERM.
- Tests added/changed: `tests/bootstrap/test_serve.cpp` (+3), `test_prompt_runner.cpp`
  (+2).
- Bench impact: none.
- Compile-budget delta: no new TU/header (extends `serve.cpp`/`serve.hpp` and
  `prompt_runner.*`); `serve.cpp` now pulls `<oran/agent/scheduler.hpp>` + tool
  registry/builtins headers (already in the bootstrap umbrella via `prompt_runner.cpp`),
  and `prompt_runner.hpp` pulls the light `<oran/agent/scheduler.hpp>`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: 2026-06-20 — CLI/channel agent loops drive the scheduler on the
  multi-worker `runtime.executor()` vs the single-strand lock-table contract; drive
  them on a per-agent strand as `--serve` now does.
- Linked release note: serve-scheduler-reaping (feature-release-notes.md).
- Remaining for the track: a triggered-work ingress (channel/webhook →
  `AutomationService::enqueue_triggered`); optional Slice D typed `serve` config block.

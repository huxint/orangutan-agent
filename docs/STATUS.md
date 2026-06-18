# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now, then read your track's row in
> [`ROADMAP.md`](ROADMAP.md) for the frontier, next step, and
> pre-dependencies. Update this file in the same commit as the history entry
> that moves it — see [`rules/docs-in-sync.md`](rules/docs-in-sync.md).
> Per-slice narrative lives in [`histories/`](histories/), not here.

## Snapshot

- **Slice:** 254 (`xmake run orangutan -- --help` reports slice 254)
- **Last completed history:**
  [`histories/2026-06/20260618-1719-serve-automation-cron-loop.md`](histories/2026-06/20260618-1719-serve-automation-cron-loop.md)
- **Active exec-plans:**
  - [`exec-plans/active/2026-06-18-runtime-service-owner.md`](exec-plans/active/2026-06-18-runtime-service-owner.md)
    — the current main line. Slice A (slice 253) shipped the `--serve` skeleton
    + IO file-view watcher auto-start; Slice B (slice 254) shipped the automation
    cron/triggered loop under `--serve`; only Slice C (scheduler idle-lock reaping)
    remains. Resolves ROADMAP Dependency Frontier #2.
  - [`exec-plans/active/2026-06-10-channel-qq-port.md`](exec-plans/active/2026-06-10-channel-qq-port.md)
    — remains active, but its next gate is externally blocked on real QQ
    credentials plus a sendable operator conversation; it was spun off from the
    completed channel-ingress plan
    ([`exec-plans/completed/2026-06-09-channel-ingress-and-adapters.md`](exec-plans/completed/2026-06-09-channel-ingress-and-adapters.md)).
  - *Recently completed:* the desktop chat-tracer plan closed 2026-06-16
    (slices 248–252 — spec 0007 acceptance 1–3) and moved to
    [`exec-plans/completed/2026-06-14-oran-desktop-chat-tracer.md`](exec-plans/completed/2026-06-14-oran-desktop-chat-tracer.md).
- **Latest completed slice:** slice 254 ships the runtime-service owner **Slice B**
  — the automation cron/triggered loop under `--serve`. When the loaded config carries
  `automation.cron.jobs[]`, `bootstrap::run_serve` builds a `RuntimeAssembly` + provider
  (live `HttpProviderBackend`, else an offline scripted `FakeProvider`), opens
  `<workspace>/.orangutan/automation.db` (`AutomationRuntime::open`), applies the mapped
  cron seeds once, builds a prompt-backed handler, and races a new
  `bootstrap::serve_automation` concern (a cancel-aware poll loop over
  `automation::AutomationService::run`) beside the IO watcher under one cancellation
  signal. With no cron jobs configured, `--serve` is exactly the slice-A watcher
  (no provider, no DB — CI-identical). `test-bootstrap` 164 cases / 1603 assertions
  (+3 / +13); release `[serve]` 7/7, full suite green under `--sanitizers=y`; offline
  smoke fires a cron job (recorded success) and exits 130 (SIGINT) / 143 (SIGTERM).
- **Next intended slice:** runtime-service owner **Slice C** — the tool-scheduler
  idle-lock reaping tick (first hoist `ToolScheduler` ownership out of
  `AgentPromptRunner`, then add a periodic `reap_idle_locks` concern inside
  `serve_body`). QQ-port 4b-ii still waits on real QQ credentials and an operator
  conversation.
- **Cross-track progress:** [`ROADMAP.md`](ROADMAP.md) — per-track frontier,
  next step, and pre-dependencies.

## Library Health

Lifted from [`QUALITY_SCORE.md`](QUALITY_SCORE.md). `STATUS.md` summarizes;
`QUALITY_SCORE.md` explains.

| Score | Areas |
| ----- | ----- |
| **A** | *(none yet — pre-v1)* |
| **B** | Architecture docs, Build system, Async model, Security defaults, Supply chain |
| **C** | Compile-time discipline, Tests, Benches, IO, Storage, Config, Bootstrap, Provider system, Tool registry, Prompt builder, Memory tiers, Permissions, Hooks, Channels, Orchestration, Automation, Desktop App, CLI, Skills, Observability, Static analysis |
| **D** | *(none)* |

## Latest Library Surfaces

- `oran-core`: 71 cases / 459 assertions.
- `oran-async`: 16 cases / 83 assertions.
- `oran-http`: 28 cases / 185 assertions.
- `oran-io`: 54 cases / 311 assertions.
- `oran-storage`: 79 cases / 1047 assertions.
- `oran-config`: 56 cases / 537 assertions.
- `oran-permission`: 89 cases / 426 assertions.
- `oran-hook`: 38 cases / 313 assertions.
- `oran-memory`: 38 cases / 841 assertions.
- `oran-automation`: 106 cases / 1849 assertions.
- `oran-channel`: 25 cases / 191 assertions.
- `oran-channel-qq` (gated, `--channel_qq=y`): 58 cases / 369 assertions.
- `oran-skill`: 27 cases / 168 assertions.
- `oran-tool`: 208 cases / 2181 assertions.
- `oran-prompt`: 10 cases / 98 assertions.
- `oran-provider`: 88 cases / 664 assertions.
- `oran-agent`: 57 cases / 10 786 assertions.
- `oran-cli`: 28 cases / 221 assertions.
- `oran-desktop`: 17 cases / 70 assertions.
- `oran-bootstrap`: 164 cases / 1603 assertions (gated `--channel_qq=y`: 156 /
  1382).

## Open Tech-Debt Rows

Lifted from [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md).
Closed entries do *not* live here — the tracker is canonical.

- 2026-05-26 — Deep-review backlog `review/deep-2026-05-26`: slices 113 and
  114 absorbed the high/medium bullets and most low-severity items.
  Remaining: a future regression test for the oran-io singleflight
  leader-cancel + cross-executor-wake fix; rebench any cache stat consumers
  that previously read `evictions_bytes` / `evictions_lru` after
  invalidations now that those evictions move to
  `EvictionReason::invalidated` (no callers do today, but a future
  observability consumer should be aware).
- 2026-05-21 — Second deep-review follow-up
  `review/deep-2026-05-21-followup`: slices 151–153 closed the config
  strictness sweep, redacted mutation hook payloads, and atomic-write
  durability. Remaining: CI xmake/test wiring after reference hardware is
  provisioned.
- 2026-05-21 — Deep-review backlog: slices 31–36, 60, 115, and 154–196
  absorbed the ranked items (see the tracker for the per-slice map).
  Remaining: sqlite-vec corpus numbers, embedding/vector ownership, and
  hybrid ranking policy/wiring stay grouped P1/P2/P3 in the tracker.
- 2026-05-20 — `scripts/check-compile-budget.sh` exists and works (slice 28)
  but is not wired into `scripts/ci.sh`. Gated on CI provisioning xmake on
  the documented reference hardware (8-core / NVMe / native Linux);
  otherwise the gate fires on environmental drift, not real regressions.
- 2026-05-17 — `FileSearch` does not yet ship ripgrep-class optimisations
  (mmap, extension-based binary skip, multi-threaded walk).
  Adequate at slice 20 (~27 µs / 4-file tree) but 3-10× slower than a tuned
  scanner on repo-scale inputs. Re-bench once `oran-agent` produces a real
  workload measurement.
- 2026-05-14 — Generated `docs/generated/config.schema.json` not yet
  implemented.
- 2026-05-14 — bench A-vs-B scenarios listed in
  `bench/<lib>/README.md` are placeholders.

## How To Update

1. The slice that lands a behavior change writes its history file.
2. The **same commit** updates this file: bump `Slice`, point
   `Last completed history` at the new file, refresh
   `Active exec-plans` (paths or `none` + reason — see
   [`PLANS_GUIDE.md`](PLANS_GUIDE.md) "When NOT To Create A Plan"),
   replace `Latest completed slice` / `Next intended slice` with the new
   frontier (move the old text nowhere — `histories/` already records it),
   refresh the test/assertion counts in "Latest Library Surfaces",
   and re-sync the tech-debt list from
   `exec-plans/tech-debt-tracker.md`.
3. If the slice moved a track's frontier, refresh that track's row in
   [`ROADMAP.md`](ROADMAP.md) in the same commit.
4. `scripts/check-status-fresh.sh` fails the build if `STATUS.md`'s
   `Last completed history` pointer is older than the newest file under
   `docs/histories/`.

## See Also

- [`ROADMAP.md`](ROADMAP.md) — per-track frontier, next steps,
  pre-dependencies.
- [`QUALITY_SCORE.md`](QUALITY_SCORE.md) — the per-area scoring rubric.
- [`releases/feature-release-notes.md`](releases/feature-release-notes.md)
  — chronological user-visible change log.
- [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md)
  — open debt rows.

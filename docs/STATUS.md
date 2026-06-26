# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now, then read your track's row in
> [`ROADMAP.md`](ROADMAP.md) for the frontier, next step, and
> pre-dependencies. Update this file in the same commit as the history entry
> that moves it — see [`rules/docs-in-sync.md`](rules/docs-in-sync.md).
> Per-slice narrative lives in [`histories/`](histories/), not here.

## Snapshot

- **Slice:** 262 (`xmake run orangutan -- --help` reports slice 262)
- **Last completed history:**
  [`histories/2026-06/20260626-1626-serve-channel-message-deadline.md`](histories/2026-06/20260626-1626-serve-channel-message-deadline.md)
- **Active exec-plans:**
  - [`exec-plans/active/2026-06-10-channel-qq-port.md`](exec-plans/active/2026-06-10-channel-qq-port.md)
    — the only active plan; its next gate is externally blocked on real QQ
    credentials plus a sendable operator conversation. It was spun off from the
    completed channel-ingress plan
    ([`exec-plans/completed/2026-06-09-channel-ingress-and-adapters.md`](exec-plans/completed/2026-06-09-channel-ingress-and-adapters.md)).
  - *Recently completed:* the runtime-service-owner plan closed 2026-06-20
    (slices 253–255 — `--serve` watcher + automation loop + scheduler idle-lock
    reaping; ROADMAP Dependency Frontier #2) and moved to
    [`exec-plans/completed/2026-06-18-runtime-service-owner.md`](exec-plans/completed/2026-06-18-runtime-service-owner.md).
    The desktop chat-tracer plan closed 2026-06-16
    ([`exec-plans/completed/2026-06-14-oran-desktop-chat-tracer.md`](exec-plans/completed/2026-06-14-oran-desktop-chat-tracer.md)).
- **Latest completed slice:** slice 262 adds a C++ owner/test per-message
  deadline for configured channel dispatch under `orangutan --serve`.
  `ServeChannelOptions::message_deadline` bounds one routed agent/reply send
  attempt; on expiry, the in-flight attempt is cancelled and the worker sends a
  fixed still-working fallback reply on the same inbound envelope. Worker metrics
  now include `message_timeouts`, while successful fallback sends still count as
  replies and failed fallback sends count as dispatch failures. No JSON config
  field, durable background rejoin, or operator-facing deadline policy was added;
  those remain deferred to a typed `serve`/channel config surface. Per-agent
  strand splitting, webhook ingress, concrete automation notifier routing, a
  durable later-reply path, and an operator-facing metrics sink remain open.
  `test-bootstrap` 183 cases / 1793 assertions; focused
  `[serve][channels][deadline]`, metrics, ordering, and invalid-options coverage
  passed; full `xmake test` 19/19 and `make ci` passed.
- **Next intended slice:** continue generic channel hardening with a durable
  deadline rejoin/later-reply path, or split the routed agent path onto per-agent
  strands where the service-level strand is still too coarse. Other viable
  follow-ups are an operator-facing metrics sink, the webhook
  adapter/producer path for non-chat triggers, concrete automation notifier
  routing, or QQ-port milestone 4b-ii once real QQ credentials and an operator
  conversation exist.
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
- `oran-config`: 58 cases / 562 assertions.
- `oran-permission`: 89 cases / 426 assertions.
- `oran-hook`: 38 cases / 313 assertions.
- `oran-memory`: 38 cases / 841 assertions.
- `oran-automation`: 107 cases / 1868 assertions.
- `oran-channel`: 26 cases / 205 assertions.
- `oran-channel-qq` (gated, `--channel_qq=y`): 58 cases / 369 assertions.
- `oran-skill`: 27 cases / 168 assertions.
- `oran-tool`: 208 cases / 2181 assertions.
- `oran-prompt`: 10 cases / 98 assertions.
- `oran-provider`: 88 cases / 664 assertions.
- `oran-agent`: 57 cases / 10 786 assertions.
- `oran-cli`: 28 cases / 221 assertions.
- `oran-desktop`: 17 cases / 70 assertions.
- `oran-bootstrap`: 183 cases / 1793 assertions (gated `--channel_qq=y`: 185 /
  1833).

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

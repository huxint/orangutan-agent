# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now, then read your track's row in
> [`ROADMAP.md`](ROADMAP.md) for the frontier, next step, and
> pre-dependencies. Update this file in the same commit as the history entry
> that moves it — see [`rules/docs-in-sync.md`](rules/docs-in-sync.md).
> Per-slice narrative lives in [`histories/`](histories/), not here.

## Snapshot

- **Slice:** 250 (`xmake run orangutan -- --help` reports slice 250)
- **Last completed history:**
  [`histories/2026-06/20260616-0200-desktop-bridge-view-model.md`](histories/2026-06/20260616-0200-desktop-bridge-view-model.md)
- **Active exec-plans:**
  - [`exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md`](exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md)
    — Slices A (Slint packaging + skeleton window, slice 248), B
    (`web`→`desktop` config migration, slice 249), and C (always-built bridge +
    view-model, slice 250) landed; Slice D (chat tracer end-to-end) is next.
  - [`exec-plans/active/2026-06-10-channel-qq-port.md`](exec-plans/active/2026-06-10-channel-qq-port.md)
    — remains active, but its next gate is externally blocked on real QQ
    credentials plus a sendable operator conversation; it was spun off from the
    completed channel-ingress plan
    ([`exec-plans/completed/2026-06-09-channel-ingress-and-adapters.md`](exec-plans/completed/2026-06-09-channel-ingress-and-adapters.md)).
- **Latest completed slice:** slice 250 ships Desktop App **Slice C** — the
  always-built `oran-desktop` bridge + view-model. A `ChatViewModel` folds a
  stream of `UiUpdate`s into renderable transcript state; a `DesktopEventSink :
  provider::EventSink` translates streamed deltas into `UiUpdate`s through a
  delivery hook; a `ChatBridge` owns the bounded UI↔runtime queues
  (`async::Channel<std::string>` prompts, `async::Channel<UiUpdate>` updates) and
  a per-turn `asio::cancellation_signal`, wiring the sink's delivery into the
  runtime→UI queue with overflow-drop accounting. `bootstrap::AgentPromptRunner`
  gains an optional injected `provider::EventSink*` so the desktop reuses the
  full loop runner. All pure C++ — no Slint — and built test-first against a fake
  provider: `test-desktop` **15 cases / 59 assertions**, `test-bootstrap` 157 /
  1581.
- **Next intended slice:** Desktop **Slice D** — the chat tracer end-to-end: the
  Slint chat UI (input, live transcript, stop) bound to the `ChatBridge`, with
  `orangutan --desktop` building the runner + bridge and opening the working
  chat. Satisfies spec 0007 acceptance criteria 1–3; manual `--desktop=y` run
  recorded. QQ-port 4b-ii remains waiting on real QQ credentials and an operator
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
- `oran-async`: 14 cases / 76 assertions.
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
- `oran-desktop`: 15 cases / 59 assertions.
- `oran-bootstrap`: 157 cases / 1581 assertions (gated `--channel_qq=y`: 149 /
  1360).

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

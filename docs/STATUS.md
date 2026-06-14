# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now, then read your track's row in
> [`ROADMAP.md`](ROADMAP.md) for the frontier, next step, and
> pre-dependencies. Update this file in the same commit as the history entry
> that moves it — see [`rules/docs-in-sync.md`](rules/docs-in-sync.md).
> Per-slice narrative lives in [`histories/`](histories/), not here.

## Snapshot

- **Slice:** 241 (`xmake run orangutan -- --help` reports slice 241)
- **Last completed history:**
  [`histories/2026-06/20260614-1656-trace-export-file-sink.md`](histories/2026-06/20260614-1656-trace-export-file-sink.md)
- **Active exec-plans:**
  - [`exec-plans/active/2026-06-10-channel-qq-port.md`](exec-plans/active/2026-06-10-channel-qq-port.md)
    — remains active, but its next gate is externally blocked on real QQ
    credentials plus a sendable operator conversation; it was spun off from the
    completed channel-ingress plan
    ([`exec-plans/completed/2026-06-09-channel-ingress-and-adapters.md`](exec-plans/completed/2026-06-09-channel-ingress-and-adapters.md)).
- **Latest completed slice:** slice 241 advances the unblocked
  Observability/trace line while QQ 4b-ii remains credential-blocked. The
  bootstrap trace exporter now supports `--trace-export-file <path>` on both
  `--trace-export <turn-id>` and bounded list mode:
  `--trace-export [--agent <name>] [--limit <n>]`. The file sink writes the
  exact same redacted JSON Lines objects that stdout mode would emit, creates
  parent directories, truncates the target file, and suppresses stdout so
  operators can hand the result to log/SIEM collectors without shell
  redirection. The command remains read-only against
  `<workspace>/.orangutan/audit.db`, rejects duplicate, empty, missing, and
  unscoped file-sink arguments, preserves invalid-limit and single-turn-filter
  validation, and still returns success with an empty file/output when a
  bounded list query has no matches. Focused results: default
  `test-bootstrap` **153 cases / 1484 assertions** and `[trace]` **16 cases /
  230 assertions**; gated QQ counts remain gated `test-bootstrap` **148 cases /
  1352 assertions** and gated `test-channel-qq` **58 cases / 369 assertions**
  from slice 238.
- **Next intended slice:** Observability v1.1 — add an explicit HTTP POST sink
  for the same single-turn and bounded trace-export query, with loopback tests,
  before considering a broader query language. QQ-port 4b-ii remains waiting
  on real QQ credentials and an operator conversation.
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
- `oran-http`: 27 cases / 148 assertions.
- `oran-io`: 54 cases / 311 assertions.
- `oran-storage`: 77 cases / 988 assertions.
- `oran-config`: 55 cases / 519 assertions.
- `oran-permission`: 89 cases / 426 assertions.
- `oran-hook`: 38 cases / 313 assertions.
- `oran-memory`: 38 cases / 841 assertions.
- `oran-automation`: 106 cases / 1849 assertions.
- `oran-channel`: 25 cases / 191 assertions.
- `oran-channel-qq` (gated, `--channel_qq=y`): 58 cases / 369 assertions.
- `oran-skill`: 27 cases / 168 assertions.
- `oran-tool`: 208 cases / 2181 assertions.
- `oran-prompt`: 10 cases / 98 assertions.
- `oran-provider`: 86 cases / 652 assertions.
- `oran-agent`: 56 cases / 10 744 assertions.
- `oran-cli`: 26 cases / 205 assertions.
- `oran-bootstrap`: 153 cases / 1484 assertions (gated `--channel_qq=y`: 148 /
  1352).

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
- 2026-05-17 — `file.search` does not yet ship ripgrep-class optimisations
  (mmap, extension-based binary skip, multi-threaded walk).
  Adequate at slice 20 (~27 µs / 4-file tree) but 3-10× slower than a tuned
  scanner on repo-scale inputs. Re-bench once `oran-agent` produces a real
  workload measurement.
- 2026-05-14 — Generated `docs/generated/config.schema.json` not yet
  implemented.
- 2026-05-14 — bench A-vs-B scenarios listed in
  `bench/<lib>/README.md` are placeholders.
- 2026-06-06 — Frontend stack choice for the desktop app shell is settled
  (Slint); the `oran-desktop` library and the `web` → `desktop` config-block
  migration are still open.

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

# Quality Score

Track quality by area so agents can prioritize the weakest parts of the system. The
table represents the *current* state of the rewrite. Update in the same PR that
materially changes an area.

## Scale

- `A` — strong coverage, stable behavior, clear docs, low operational risk.
- `B` — acceptable but has known gaps.
- `C` — works but needs targeted hardening.
- `D` — fragile, under-specified, or not yet implemented.
- `—` — not applicable (e.g., shaped only).

## Current State (Pre-v1)

| Area                     | Score | Why | Next Step |
| ------------------------ | ----- | --- | --------- |
| Architecture docs        | B     | Top-level map + design docs drafted. Some sections await concrete code. | Land MVP code; back-fill diagrams. |
| Build system             | B     | xmake skeleton lands in slice 0; GCC 16.1 toolchain detected, C++26 enforced, `compile_commands.json` autoupdates. | Land per-library `check-compile-budget.sh` numbers + Clang secondary toolchain. |
| Compile-time discipline  | C     | Budgets captured; `oran-core`, `oran-async`, `oran-io`, and `oran-storage` build under the current target budget, but no per-TU baseline file exists yet. | Land per-library `check-compile-budget.sh` numbers. |
| Test framework           | C     | Catch2 v3 buckets green for `oran-core` (8 cases / 47 assertions), `oran-async` (8 cases / 38 assertions), `oran-io` (8 cases / 33 assertions), `oran-storage` (50 cases / 582 assertions), `oran-config` (5 cases / 49 assertions), `oran-cli` (5 cases / 30 assertions), and `oran-bootstrap` (8 cases / 34 assertions). | Add first permission/tool tests when those libraries land. |
| Bench harness            | C     | nanobench buckets green for `oran-core`, `oran-async`, `oran-io`, `oran-storage`, `oran-config`, `oran-cli`, and `oran-bootstrap`; storage covers inserts, compiled-span migrations, SQL-file migration load+run, direct-vs-pool acquire, fresh-vs-cached prepare, pool fresh-vs-cached prepare, and raw-pool-vs-session-repository append/load; config covers parse-vs-file-load, CLI covers prompt-vs-REPL dispatch, and bootstrap covers default-vs-explicit config startup. No baseline JSON or regression gate yet. | Wire `scripts/bench-compare.sh` to a real baseline file. |
| Async model              | B     | `Runtime`, `Awaitable<T>`, cancel-aware `sleep_for`, and bounded `Channel<T>` are implemented with tests and a bench; `oran-storage::Pool` now consumes the executor for async writer/reader acquisition. Runtime signal integration and mailbox policy are still downstream. | Add cancellation-latency bench coverage for the first orchestration workload. |
| IO runtime               | C     | `oran-io` file/directory MVP is implemented with tests and a bench; subprocess, glob, pipe, signal, and watcher APIs are still downstream. | Add subprocess/signal helpers after permission and hook surfaces are ready to wrap them. |
| Storage / DBs            | C     | Expected-only SQLite `Connection` / `Statement` core, migration runner with SQL-file directory loading, async writer/reader `Pool`, per-slot pool `StatementCache`, standalone per-connection `StatementCache` (LRU, hit/miss/eviction counters, transient overflow), and `SessionRepository` are implemented with WAL/foreign-key setup, schema versioning, RAII leases, FIFO waiter resumption, cancellation, tests, and benches. Memory/automation/audit repositories and packaged migration asset lookup are still downstream. | Add migration asset packaging or the next domain repository on top of the cached pool surface. |
| Config                   | C     | Expected-only JSON loader is implemented with typed runtime/profile/route/session/web fields, recursive env substitution, strict unknown-root handling, `config.example.json`, tests, and a bench. Generated schema, secret crypto, and broad typed section models are still downstream. | Generate JSON Schema and add secret-field support. |
| Bootstrap                | C     | Process entry now routes through `oran-bootstrap`, loads explicit or default config, falls back to built-in defaults when the default file is absent, and hands CLI args to `oran-cli`. Runtime assembly, signal handling, and provider startup are still downstream. | Build runtime assembly around loaded config. |
| Provider system          | C     | Layered design captured. | First adapter (Anthropic Messages). |
| Tool registry            | C     | Design captured. | First built-ins (file + shell + memory). |
| Memory tiers             | C     | Tier design captured. | Long-term FTS5 v1. |
| Permissions              | C     | Engine design captured; ctre→re2 swap planned. | Implement engine. |
| Hooks                    | C     | Enumerated lifecycle. | Shell + in-proc sinks. |
| Channels                 | C     | Trait + capability matrix designed. | QQ port + webhook adapter. |
| Orchestration / teams    | C     | Coordination strategies designed. | LeaderWorker + Pipeline. |
| Automation               | C     | Job categories designed. | Cron + periodic + triggered. |
| Web UI                   | C     | Routes + token-auth designed. | Single-page chat. |
| CLI                      | C     | `oran-cli` implements deterministic pre-agent-loop REPL and `--prompt` single-shot shells with tests and a bench. Provider-backed streaming is still downstream. | Connect CLI prompts to `oran-agent` once provider/tool foundations land. |
| Skills                   | D     | Spec drafted; no implementation. | Loader + watcher + `skill.invoke`. |
| Observability            | D     | Logging shim designed. | Metrics endpoint. |
| Security defaults        | B     | Captured. | Implement secret rotation. |
| Supply chain             | B     | Workflows pinned, lockfile present. | Add OSV scan once xmake lock is real. |
| Static analysis          | C     | `-fanalyzer` wiring shipped (rule + xmake option); focused `oran-io` and `oran-storage` analyzer builds pass locally, but analyzer CI remains a planned nightly gate. | Run/record analyzer coverage again for later descriptor-heavy code. |

## Cadence

Reviewed at every v1.x milestone. PRs that move a row's score change it (up or down)
in the same change.

## How To Move A Row Up

- D → C: implementation exists + smoke test passes.
- C → B: full test bucket green, design doc current.
- B → A: bench coverage in place, ≥ 30 days of stable operation, no known issues
  in the tech-debt tracker for that area.

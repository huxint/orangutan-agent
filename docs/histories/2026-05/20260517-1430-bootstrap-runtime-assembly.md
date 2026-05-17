## [2026-05-17 14:30] | Task: bootstrap RuntimeAssembly wiring (ApprovalBroker + AuditSink)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — single-session slice that fits the
  `Next intended slice` bullet in [`STATUS.md`](../../STATUS.md)
  ("agent loop scaffolding with `ApprovalBroker` + `AuditSink`
  ownership through `oran-bootstrap`"). Per
  [`PLANS_GUIDE.md`](../../PLANS_GUIDE.md) "When NOT To Create A
  Plan", a single-slice slice closed in one session does not
  open a plan.

### User Query

> 详细了解项目目标，查看当前项目真实进度, 推进项目代码的实现.
> (Understand the project's goals, inspect the current real
> progress, push the project's code forward.)

Reading `docs/STATUS.md` revealed two parallel "Next Step" notes
pointing at the same wiring:

- Bootstrap row: "Wire a per-process `ApprovalBroker` +
  `StorageAuditSink` into runtime assembly so the upcoming agent
  loop inherits both."
- Permissions row: "Wire `StorageAuditSink` into `oran-bootstrap`
  so the upcoming agent loop inherits a sink handle; close
  `0008-permissions.md` criterion 1; first tool-registry
  built-ins."

This slice picks up the bundling step the rest of `0008-permissions.md`
criterion 1 was waiting on.

### Changes Overview

- **New library surface:** `bootstrap::RuntimeAssembly` — a move-only
  value type bundling a fresh `permission::ApprovalBroker` (per-process
  HMAC key per criterion 5) and the active `permission::AuditSink`
  (`StorageAuditSink` over an internal `storage::Pool` +
  `storage::AuditRepository` when audit is enabled, `NullAuditSink`
  otherwise). One `build(workspace, runtime_executor, options)`
  factory; pImpl hides storage so the heap-allocated members keep
  stable addresses across `RuntimeAssembly` moves and the captured
  `Pool&` / `AuditRepository&` pointers never dangle.
- **Bootstrap integration:** `bootstrap::run` now builds the
  assembly between config load and CLI handoff using the loaded
  config's `runtime.workers` for the `async::Runtime`'s
  `io_workers`. The banner prints a `runtime assembly ready: …`
  line. The assembly is dropped at function exit for now; the
  agent-loop slice will own it for the lifetime of the process.
- **Audit default:** `bootstrap::run` defaults to `audit_enabled=false`
  so `orangutan` stays runnable from any CWD. A tech-debt row
  records that the migration assets are still on-disk; the
  `--audit-init` operator command keeps the storage path exercised.
- **Tests:** `tests/bootstrap/test_runtime_assembly.cpp` covers the
  null-sink default, storage-sink default-path provisioning,
  explicit-path override, idempotent re-build, end-to-end record
  through both sink backends, broker round-trip, and broker
  cross-tool rejection. Bucket grows to 20 cases / 75 assertions
  (+9 cases / +28 assertions).
- **Bench:** `bench/bootstrap/scenarios/runtime_assembly_build.cpp`
  ships an A-vs-B (`assembly_build_with_audit` ~212 µs vs.
  `assembly_build_without_audit` ~400 ns; the audit pipeline
  costs ~211 µs per process startup, dominated by SQLite open +
  migration + `Pool::open`). The scenario lowers
  `minEpochIterations` to 500 because the with-audit path is
  ms-scale; the µs-scale config-loading scenarios stay at 150000
  because the assembly registration runs last.
- **Slice tag bump:** `kVersion` is now `2.0.0-slice14`.

### Design Intent

**Why the assembly does not own the `async::Runtime`.** `async::Runtime`
is the per-process executor source; the agent loop, the channels,
and the eventual web UI all want to dispatch onto it. Putting the
runtime inside the assembly would force callers that don't want
audit (tests, one-shot operator commands) to either pay for a full
thread-pool boot or to invent a "minimum viable runtime". Keeping
the executor as a build-time parameter lets tests substitute an
`asio::io_context` while the agent loop substitutes
`async::Runtime::executor()`. The assembly's only external
dependency is "an executor the long-lived audit `Pool` can
dispatch onto".

**Why `build()` is synchronous despite migrating the audit DB.** The
migration step needs a driven executor — `AuditRepository::migrate`
is an `Awaitable<...>`. The caller's `runtime_executor` parameter is
*not running* during `build()` (the agent loop drives it later via
`Runtime::run()`). A migration coroutine posted to it would never
progress. The natural fix would be to make `build()` a coroutine
too, but that pushes the caller to wrap every call in `co_spawn`
even though everything else in `oran-bootstrap` is sync. The
implementation therefore drives migration on a *temporary*
`asio::io_context`, then reopens a long-lived `Pool` against the
caller-supplied executor. The DB on disk is identical; the
long-lived pool just sees a migrated schema. This is the same
pattern `run_audit_init` already uses, so the two code paths share
their fallibility envelope. Cost: one extra `Pool::open` per build
(~hundreds of µs), measured in the bench.

**Why audit defaults to disabled in `bootstrap::run` for now.** The
storage migration runner finds its SQL files by walking up from
CWD until it sees `src/oran-storage/migrations/audit/`. That's
fine inside the repository (xmake's `set_rundir(root)` pins CWD),
but a user running `orangutan` from another directory hits a
`not_found` error. The right fix is to package the migration
strings into `oran-storage` itself (a tech-debt row was added),
but that is a separate slice. Defaulting `audit_enabled=false`
keeps the slice 14 wiring honest without making the binary
unusable from outside the repo. The `--audit-init` operator
command stays storage-backed because it runs from inside the
repo today.

**Why hold members behind `unique_ptr` instead of `std::optional`.**
The assembly's `Pool` is referenced by the `AuditRepository`
(stored `Pool*`); the `AuditRepository` is referenced by the
`StorageAuditSink` (stored `AuditRepository*`). A move of the
assembly must not invalidate those pointers. Using `unique_ptr<T>`
inside a pImpl keeps every dependent's address stable across the
assembly's moves: only the `unique_ptr` itself is moved; the
heap object stays put. `std::optional<T>` would move the underlying
object on assembly move, breaking the captured pointers.

**Why `RuntimeAssembly` lives in `oran-bootstrap`, not `oran-permission`.**
The assembly composes objects from `oran-permission` *and*
`oran-storage` *and* `oran-async`. The natural home is the place
that already depends on all three — `oran-bootstrap`. Putting
the type in `oran-permission` would force `oran-permission` to
add an `oran-async` dependency for an asio executor parameter; it
would also blur the rule that permission is "the runtime layer
above storage" — the assembly is "bootstrap composition glue", not
permission logic.

### Files Modified

- `include/oran/bootstrap/runtime_assembly.hpp` — new file.
- `src/oran-bootstrap/runtime_assembly.cpp` — new file.
- `include/oran/bootstrap.hpp` — umbrella header now re-exports
  the assembly header.
- `src/oran-bootstrap/bootstrap.cpp` — `run()` builds the assembly
  between config load and CLI handoff; `kVersion` bumped to
  `2.0.0-slice14`; new `<algorithm>` / `<cstddef>` / `<cstdint>`
  includes for `std::max` and the `int64_t→size_t` cast on
  `runtime.workers`.
- `tests/bootstrap/test_runtime_assembly.cpp` — new file (9 cases).
- `bench/bootstrap/scenarios/runtime_assembly_build.cpp` — new file
  (one A-vs-B scenario).
- `bench/bootstrap/main.cpp` — registers the new scenario after
  `register_config_startup`.
- `docs/STATUS.md` — slice 14, history pointer, bootstrap
  assertion count, new tech-debt row.
- `docs/QUALITY_SCORE.md` — Bootstrap, Permissions, Test framework,
  and Bench harness rows refreshed.
- `docs/ARCHITECTURE.md` — slice-status preamble + `oran-bootstrap`
  inventory row mention the assembly.
- `docs/design-docs/permissions-and-hooks.md` — audit-pipeline
  paragraph now records the bootstrap-assembly close.
- `docs/product-specs/0008-permissions.md` — criterion 1 closing
  note extended to mention the assembly.
- `docs/releases/feature-release-notes.md` — new row.
- `docs/exec-plans/tech-debt-tracker.md` — new row for the
  CWD-based migration directory lookup.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice + history pointer + assertion counts +
  tech-debt sync.
- `docs/QUALITY_SCORE.md` — Bootstrap, Permissions, Test framework,
  Bench harness rows.
- `docs/ARCHITECTURE.md` — slice-status preamble + `oran-bootstrap`
  library row.
- `docs/design-docs/permissions-and-hooks.md` — audit pipeline
  paragraph closing the assembly wiring.
- `docs/product-specs/0008-permissions.md` — criterion 1.
- `docs/releases/feature-release-notes.md` — new row.
- `docs/exec-plans/tech-debt-tracker.md` — migration-asset CWD row.
- `docs/histories/2026-05/20260517-1430-bootstrap-runtime-assembly.md`
  — this file.

### Validation

- Commands run:
  - `xmake build orangutan` — clean build.
  - `xmake test` — all 8 buckets green; bootstrap bucket is now
    20 cases / 75 assertions (+9 cases / +28 assertions).
  - `xmake build bench-bootstrap && xmake run bench-bootstrap` —
    bench produces stable numbers; A/B shape lands above.
  - Smoke test: `cd /tmp/elsewhere && orangutan --prompt hi` —
    prints the slice-14 banner and reaches the CLI handoff
    without trying to migrate audit.db from an unrelated CWD.
- Tests added/changed: 9 new bootstrap cases.
- Bench impact: one new bootstrap A-vs-B
  (`assembly_build_with_audit` ~212 µs vs.
  `assembly_build_without_audit` ~400 ns).
- Compile-budget delta: a new TU in `oran-bootstrap`; the assembly
  pulls `oran/permission.hpp` + `oran/storage.hpp` (already
  reachable in the bootstrap library). Clean rebuild was 8.4 s.

### Follow-ups

- Issues to file: none beyond the tech-debt row.
- Tech-debt entries: 2026-05-17 audit-migration-asset CWD lookup
  row added to `docs/exec-plans/tech-debt-tracker.md`.
- Linked release note: 2026-05-17 `bootstrap-runtime-assembly`
  row in `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: the next slice that wires
  the agent loop's per-iteration `Decision → AuditEvent` record
  call should expect `RuntimeAssembly::audit_sink()` and
  `RuntimeAssembly::approval_broker()` as the two reference-typed
  inputs.

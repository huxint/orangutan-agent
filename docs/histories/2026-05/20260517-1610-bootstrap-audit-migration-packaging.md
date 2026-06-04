## [2026-05-17 16:10] | Task: package audit migration assets via C++26 #embed

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — single-session slice that fits the
  `Next intended slice` bullet in [`STATUS.md`](../../STATUS.md)
  ("package the audit migration assets so `bootstrap::run` can
  default to `audit_enabled=true` without a CWD walk"). Per
  [`PLANS_GUIDE.md`](../../PLANS_GUIDE.md) "When NOT To Create A
  Plan", a single-slice slice closed in one session does not open
  a plan.

### User Query

> 详细了解项目目标，查看当前项目真实进度, 推进项目代码的实现.
> (Understand the project's goals, inspect the current real
> progress, push the project's code forward.)

Slice 14 wired `bootstrap::RuntimeAssembly` but left
`bootstrap::run` defaulting to `audit_enabled=false` because the
audit migration runner walked up from CWD looking for
`src/oran-storage/migrations/audit/`. That made the binary
useless outside the repo tree. This slice closes the tech-debt
row and lets the assembly default to `audit_enabled=true`.

### Changes Overview

- **New TU `src/oran-storage/migration_assets.cpp`** declaring the
  storage-internal accessors `built_in_audit_migrations()` and
  `built_in_session_migrations()` that return
  `std::span<const Migration>`. Each SQL string is reached at
  compile time via a `constexpr unsigned char[]` initialized with
  C++26 `#embed` of the canonical
  `migrations/audit/0001-audit-initial.sql` and
  `migrations/sessions/0001-sessions-initial.sql`. The `Migration`
  table lives behind a function-local
  `static const std::array<Migration, 1>` so the `std::string`
  init stays off the global-constructor critical path.
- **Header surface:** `include/oran/storage/migrations.hpp` now
  declares the two accessors next to the existing
  `run_migrations` / `run_migrations_from_directory` family.
- **Repositories use the built-ins by default.**
  `AuditRepository::migrate` and `SessionRepository::migrate`
  now branch: empty `options_.migrations_directory` →
  `run_migrations(connection, built_in_*_migrations())`; non-empty
  → the existing disk-load path. Both repositories drop their
  `resolve_*_migrations_directory` CWD-walking helpers and the
  `<filesystem>` / `kDefaultAuditMigrationsDirectory` /
  `kDefaultSessionMigrationsDirectory` includes that fed them.
- **Bootstrap flips audit default to enabled.**
  `bootstrap::run` constructs the assembly with the default
  `RuntimeAssemblyOptions{}` (which has `audit_enabled=true`).
  The CWD-related preamble comment is replaced with a
  slice-15-aware one. `kVersion` bumped to `2.0.0-slice15`.
- **CWD-independence test.** New
  `tests/bootstrap/test_runtime_assembly.cpp` case
  *"RuntimeAssembly::build provisions audit.db from a non-source
  CWD"* chdirs to a tempdir via an RAII `CwdGuard` and asserts the
  assembly still provisions `<workspace>/.orangutan/audit.db`.
  Bucket grows to 21 cases / 78 assertions (+1 case / +3
  assertions).
- **Tech-debt row removed.** The
  `2026-05-17 storage / bootstrap` "Audit migrations directory is
  found by walking up from CWD" row is dropped from
  `docs/exec-plans/tech-debt-tracker.md`; `STATUS.md` is in sync.

### Design Intent

**Why `#embed` instead of inlining the SQL as a raw string in a
new `.cpp` file.** Inlining duplicates the SQL between a `.sql`
file (which devs / sqlite tools / LSP know how to operate on)
and a `.cpp` raw string literal. The two sources can drift
silently. `#embed` keeps the `.sql` file as the canonical
artefact and lets the compiler bind the binary to it at build
time. The cost is zero — the embed expands to integer constants
fed into an `unsigned char[]`.

**Why a `constexpr unsigned char[]` plus a runtime `std::string`
in a function-local static, instead of `constexpr std::string_view`
or `inline constexpr std::array<char, N>`.** `#embed` produces
integer constants; an array of `unsigned char` is well-defined
even for bytes ≥ 128 (the SQL files are pure ASCII today but
that property would tilt on the first non-ASCII migration).
`std::string_view{ptr, N}` constructed via `reinterpret_cast`
is not a constant expression, so going `constexpr` end-to-end
would need either a `char[]` (subject to implementation-defined
narrowing on non-ASCII bytes) or `std::bit_cast` (which doesn't
work across pointer types). The function-local `static const
std::array<Migration, 1>` initialises lazily on first call,
thread-safe per [stmt.dcl]/4, and the conversion from
`unsigned char*` to `const char*` is contained inside a single
`to_sql_string` helper that documents the intent.

**Why keep the disk-override path (`migrations_directory`).**
Two reasons: the existing
`AuditRepository::migrate accepts an explicit migration directory`
test exercises the disk path with a custom SQL file, and the
override is a useful dev-time escape hatch (e.g. authoring a new
migration against a tempdir without recompiling). The fall-back
is the empty default — the common case skips disk entirely.

**Why drop the CWD-walking helpers fully instead of keeping them
as the empty-options default.** Once the built-ins are the
default for empty `migrations_directory`, the helpers' only
remaining purpose is to look up the CWD-walked path that the
built-ins replace. Keeping dead resolver code would invite
future agents to "fix" the walk instead of using the built-ins.
Removing the helpers (and their `<filesystem>` /
`kDefaultAuditMigrationsDirectory` /
`kDefaultSessionMigrationsDirectory` machinery) makes the
intended path single — the empty default is the built-in.

**Why a CWD-independence test instead of just trusting the
smoke run.** The smoke run (`cd /tmp && orangutan --prompt …`)
proves the binary works from `/tmp` today, but it doesn't keep
working if a future slice quietly re-adds a CWD-relative lookup.
The `CwdGuard`-based unit test makes the property a regression
gate; if anyone re-introduces a CWD walk, the test fails inside
`xmake test` long before the smoke run is repeated.

### Files Modified

- `include/oran/storage/migrations.hpp` — declare
  `built_in_audit_migrations()` /
  `built_in_session_migrations()`.
- `src/oran-storage/migration_assets.cpp` — new file (the
  `#embed`-backed accessors).
- `src/oran-storage/audit_repository.cpp` — drop
  `resolve_audit_migrations_directory` +
  `kDefaultAuditMigrationsDirectory` + `<filesystem>` include;
  branch on `options_.migrations_directory` empty vs. non-empty.
- `src/oran-storage/session_repository.cpp` — same shape as
  audit.
- `src/oran-bootstrap/bootstrap.cpp` — drop the
  `audit_enabled=false` override; bump `kVersion` to
  `2.0.0-slice15`; rewrite the assembly preamble comment.
- `tests/bootstrap/test_runtime_assembly.cpp` — add `CwdGuard`
  helper + the new CWD-independence test case.
- `docs/STATUS.md` — slice 15, history pointer, bootstrap
  assertion count refresh, drop the closed tech-debt row from
  the snapshot list.
- `docs/QUALITY_SCORE.md` — Storage / Bench harness / Bootstrap
  / Permissions / Test framework rows refreshed.
- `docs/ARCHITECTURE.md` — slice-status preamble + storage row
  + bootstrap row reflect the packaged migrations.
- `docs/design-docs/permissions-and-hooks.md` — audit-pipeline
  paragraph records the slice-15 packaging and the new bench
  numbers.
- `docs/exec-plans/tech-debt-tracker.md` — closed audit-migration
  row removed.
- `docs/releases/feature-release-notes.md` — new top row.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice + history pointer + assertion counts +
  tech-debt sync.
- `docs/QUALITY_SCORE.md` — Storage / Bench harness / Bootstrap /
  Permissions / Test framework rows.
- `docs/ARCHITECTURE.md` — slice-status preamble + storage row +
  bootstrap row.
- `docs/design-docs/permissions-and-hooks.md` — audit pipeline
  paragraph.
- `docs/exec-plans/tech-debt-tracker.md` — drop the closed row.
- `docs/releases/feature-release-notes.md` — new row.
- `docs/histories/2026-05/20260517-1610-bootstrap-audit-migration-packaging.md`
  — this file.

### Validation

- Commands run:
  - `xmake build orangutan` — clean (re-compiles
    `migration_assets.cpp` + the two repositories + bootstrap).
  - `xmake test` — all 8 buckets green; bootstrap bucket is now
    21 cases / 78 assertions (+1 case / +3 assertions).
  - `xmake build bench-bootstrap && xmake run bench-bootstrap` —
    `assembly_build_with_audit` ~202 µs (down from ~212 µs
    pre-`#embed`), `assembly_build_without_audit` ~400 ns
    (unchanged).
  - Smoke test: `cd /tmp && orangutan --prompt smoke-from-tmp` —
    prints the slice-15 banner with
    `runtime assembly ready: audit=enabled (./.orangutan/audit.db)`
    and a real 28 KiB `audit.db` lands next to it.
- Tests added/changed: 1 new bootstrap case (CWD-independence).
- Bench impact: `assembly_build_with_audit` shaves ~10 µs by
  removing the CWD-scan.
- Compile-budget delta: one new TU in `oran-storage`
  (`migration_assets.cpp`); the `#embed` operator is a
  compile-time read of two small SQL files, so wall time is
  unchanged at the granularity the budget cares about. Clean
  rebuild was ~8 s.

### Follow-ups

- Issues to file: none.
- Tech-debt entries: one removed (audit-migration CWD-lookup),
  none added.
- Linked release note: 2026-05-17
  `bootstrap-audit-migration-packaging` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: when a third migration
  file lands (memory, automation, …), add the `#embed` block +
  `built_in_*_migrations()` accessor next to the existing two,
  and have the new repository's `migrate()` reach for the
  built-in by default the same way `AuditRepository::migrate` /
  `SessionRepository::migrate` do today.

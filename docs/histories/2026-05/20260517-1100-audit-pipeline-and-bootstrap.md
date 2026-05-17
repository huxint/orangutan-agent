## [2026-05-17 11:00] | Task: oran-storage::AuditRepository + audit.db schema (slice toward 0008 criterion 1)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — three-commit slice that lands the audit
  pipeline end-to-end and closes `docs/product-specs/0008-permissions.md`
  criterion 1 ("a tool call whose input matches a `deny` rule
  returns `Error::permission_denied` and is recorded in audit").
  The next-intended-slice bullet in `STATUS.md` is retired in
  the final commit of this slice.

### User Query

> 详细了解项目目标，查看当前项目真实进度, 推进项目代码的实现.
> 你这一次推进应该是能够实现 3 个commit左右. 不要盲目实现, 需要凭借
> 客观事实, 良好的代码工程和查阅网上资料进行实现 ultrathink.

The user also reminded mid-task:

> 文档和修改是同步提交的，而不是说修改完提交在写文档再提交

…so every commit in this slice carries the docs that document its
landing — there is no docs-only tail commit.

### Changes Overview

This entry is updated in each of the slice's three commits as the
work lands. The slice is built bottom-up: storage first (no permission
deps), then the permission interface on top, then the bootstrap wiring
+ slice-tag bump.

- **Commit 1** — `oran-storage::AuditRepository`. Schema migration
  `src/oran-storage/migrations/audit/0001-audit-initial.sql` lays
  down the `audit_events` table mirroring the columns the
  upcoming `permission::AuditEvent` writes
  (`scope_key`/`agent_key`/`tool_name`/`identity`/`verdict`/
  `outcome`/`reason` plus the optional `input_hash_hex` and a
  free-form `metadata_json`). The repository surface adds
  `AppendAuditEventRequest`, `AuditEventRecord`,
  `ListAuditEventsOptions`, and the `AuditRepository` class
  with `migrate`/`append_event`/`list_events`/`count_events`
  built on the async writer/reader `Pool`, mirroring
  `SessionRepository`'s shape. 8 new tests +
  `bench-storage/audit_repository` (raw pool A/B vs. typed
  repository over a 64-event batch).

### Design Intent

**Why a domain repository before the permission sink.** The audit
table is just storage from `oran-storage`'s perspective. Splitting
the schema + repository away from the permission-side sink keeps
`oran-permission` free to define an abstract `AuditSink` interface
that any future implementation (storage-backed, fire-and-forget
webhook, in-memory test double) can satisfy without dragging in
storage. The storage-backed adapter then lives wherever the
bootstrap layer wants to compose it, not inside the permission
public surface.

**Why columns mirror the eventual `AuditEvent` 1:1.** The sink's
job is to translate one struct into one row. Putting any
massaging at the storage boundary (e.g. encoding `Verdict` as a
small int code rather than text) would either leak the permission
enum into storage or require a translation table that nobody
benefits from. Wire-spelling text is what the rest of the
codebase already does (e.g. `core::Role` round-trips through
`session_messages.role` as `core::enum_name(...)` text).

**Why `reason` is `NOT NULL` but `input_hash_hex` is nullable.**
Every decision the engine emits carries a reason — either a
`Decision::reason` string or a broker `Error::with("reason",
...)` context entry, never a blank. `input_hash_hex` only exists
for callsites that compute SHA-256(input): the ask/approval flow
does it for free (the authority already needs it), but the raw
allow/deny path skips it. Storing `NULL` keeps the column truthful
about "we didn't compute this" instead of inventing a sentinel.

**Why `created_at` is stamped from SQLite, not from the caller.**
Every existing repository (`SessionRepository`) does the same
(`strftime('%Y-%m-%dT%H:%M:%fZ', 'now')` in the VALUES clause).
A single source of truth across audit + sessions means cross-table
joins (forensic queries that correlate a session message with the
audit decision that authorized the tool that produced it) order
the same way regardless of where the runtime is.

**Why three indexes (scope/agent/outcome) and not just the
scope-by-time one.** Per `docs/design-docs/secrets-and-state.md`
"Identity And Scope", every audit query starts with a `scope_key`
filter to prevent cross-scope leakage in multi-tenant runtimes —
that's the always-on hot path index. Adding `agent_key` and
`outcome` covers the documented "operator wants to list every
`deny` decision" and "show me what agent X did" flows without
re-running planner work; the additional B-tree cost is tiny
relative to the wins on those reads.

**Why the bench is an A/B against a raw pool path and not a
solo benchmark.** Every repository in the project ships against
the raw-pool baseline (`session_repository.cpp`); operators get
a stable yardstick for "how much did the abstraction cost." On
this run the raw path costs ~992 µs per 64-event batch and the
repository costs ~1219 µs (overhead ~23%, ~3.5 µs per event over
~15.5 µs raw) — the typed request/response copies are the cost,
and they buy compile-time-checked column ordering and column
type, which a stringly-typed raw path silently corrupts on
schema drift.

**Why one history entry is updated across all three commits
rather than three separate entries.** Per
[`docs/HISTORY_GUIDE.md`](../../HISTORY_GUIDE.md), a task that
spans multiple rounds updates the same history file. The slice
is a single task ("close 0008 criterion 1"); the commits are
just its internal slicing.

### Files Modified (Commit 1)

- `src/oran-storage/migrations/audit/0001-audit-initial.sql` —
  new `audit_events` table + three scoped indexes.
- `include/oran/storage/audit_repository.hpp` — new public
  surface: `AppendAuditEventRequest`, `AuditEventRecord`,
  `ListAuditEventsOptions`, `AuditRepositoryOptions`,
  `AuditRepository` (`migrate` / `append_event` /
  `list_events` / `count_events`).
- `src/oran-storage/audit_repository.cpp` — impl. Same
  validation + statement-cache shape as `SessionRepository`.
  Build-up of the dynamic `WHERE` clause is parameterized
  (cache-friendly across repeat callers with the same filter
  shape).
- `include/oran/storage.hpp` — re-export the new header.
- `tests/storage/test_audit_repository.cpp` — 8 cases: migrate
  + accepts explicit migration directory, round-trip, null
  `input_hash_hex` preserves SQL `NULL`, list/filter ordering
  by id-desc with secondary filters, validation rejects
  empty required fields, defensive parser refuses NULL-reason
  rows when a maintainer drops the schema constraint, missing
  scope returns empty.
- `bench/storage/scenarios/audit_repository.cpp` — new A/B
  scenario.
- `bench/storage/main.cpp` — registers the new bucket.
- `bench/storage/README.md` — documents the bucket.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — `oran-storage` test counts (52 → 60
  cases, 606 → 702 assertions) + tech-debt list note.
- `docs/QUALITY_SCORE.md` — `Storage / DBs` row "Why" /
  "Next Step" refreshed to mention `AuditRepository`;
  `Bench harness` row extended with the new audit A/B
  scenario.
- `docs/ARCHITECTURE.md` — `oran-storage` library inventory
  description extended to mention the new audit repository.
- `docs/design-docs/secrets-and-state.md` — Storage repositories
  list extended to call out `AuditRepository` as the third
  domain repository (after sessions; memory/automation pending).
- `docs/releases/feature-release-notes.md` — new
  `audit-db-storage` row.
- `docs/histories/2026-05/20260517-1100-audit-pipeline-and-bootstrap.md`
  — this file.

### Validation

- Commands run:
  - `xmake build oran-storage test-storage` — clean.
  - `./build/linux/x86_64/release/test-storage` — 60 cases /
    702 assertions, all green.
  - `xmake build bench-storage && xmake run bench-storage` —
    new `storage.audit_raw_pool_append_list` ~992 µs and
    `storage.audit_repository_append_list` ~1219 µs (~23%
    typed-repository overhead, ~3.5 µs per event).
  - `xmake test` (all 8 buckets) — green.
- Tests added/changed: 8 new storage cases (52 → 60 cases,
  606 → 702 assertions in storage).
- Bench impact: new `bench-storage/audit_repository` scenarios;
  existing scenarios unchanged.
- Compile-budget delta: `audit_repository.cpp` is a new TU; the
  header pulls only stdlib + the in-repo `migrations`/`pool`
  forward decls (already in PCH transitively). No PCH change.

### Follow-ups

- Issues to file: none.
- Tech-debt entry: none.
- Linked release note: `audit-db-storage` row added to
  `feature-release-notes.md` for commit 1.
- Next commits in this slice (still in this history entry):
  - Commit 2 — `permission::AuditSink` interface +
    `AuditEvent` value type + `NullAuditSink` /
    `RecordingAuditSink` + storage-backed adapter.
  - Commit 3 — wire the broker + audit sink into
    `oran-bootstrap`, bump slice tag 12 → 13, close
    `0008-permissions.md` criterion 1 with a pointer back
    here.

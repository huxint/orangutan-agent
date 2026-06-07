# Storage Runtime

`oran-storage` is the expected-only SQLite foundation used by sessions, memory,
automation, audit logs, and configuration metadata. It owns the SQLite dependency and
does not expose `sqlite3.h` from public headers.

> **Storage status (2026-06-07):** `oran-storage` ships `Connection`, `Statement`,
> typed binding/stepping/column readers (including BLOB bind/read for trace ids),
> WAL + foreign-key setup, a simple `query`
> helper, the synchronous `run_migrations` runner plus SQL-file migration
> loading, compile-time embedded audit/session migrations plus the audit-db
> trace migration via C++26 `#embed`,
> an async writer/reader `Pool` driven by `oran-async` executors with one
> `StatementCache` per writer or reader slot, and the standalone per-connection
> `StatementCache` with LRU eviction. `SessionRepository` persists opaque
> session-message JSON through the cached pool surface (with `role` typed as
> `core::Role` at the API boundary). `AuditRepository` persists permission
> decision rows in `audit_events` with append/list/count plus slice-67
> `update_event_metadata` so post-result usage metadata can enrich the same
> audit row without appending a second decision. Slice 93 adds the
> `audit_events.event_kind` discriminator (audit DB version 4) so ordinary
> permission rows keep `event_kind=permission_decision` while spec-0018
> hook observability rows use `event_kind=hook_publish`; `AuditRepository`
> exposes the field on append/update/list records and can filter
> `list_events` by it. Slice 78 adds
> `TraceRepository`, the spec-0018 `trace_turns` schema, and
> `built_in_trace_migrations()` for redacted per-turn rows keyed by 16-byte BLOB
> ids. Slice 79 adds the audit DB version-3 `audit_events.parent_turn_id`
> column and the shared `core::TurnId` / `TraceId` value shape so tool and
> hook-publish audit rows can join back to trace rows. Slice 80 adds the first agent-loop writer:
> terminal-success fake-provider turns can append one body-free `trace_turns`
> row through `TraceRepository` before returning to the caller. Memory-tier
> schemas live above the pool in `oran-memory`; slice 189 adds the automation
> retention job/run repository above the same pool in `oran-automation`, and
> slice 190 adds the caller-driven service tick above that repository. Slice
> 191 adds optional advisory hook publication above the same automation service;
> slice 192 adds a caller-owned `AutomationRuntime` handle that opens the pool
> and runs automation migrations above storage. None of these add storage-owned
> automation schema or audit rows.
> `oran-storage` stays the generic SQLite, migration, and pooling substrate.
> Backups remain future work. Slice 127 adds
> trace-derived provider usage rollups grouped by UTC day, agent, route profile,
> and route model; these sum the usage/cost fields already stored on
> `trace_turns` and do not yet compute cost from profile pricing. Slice 176 adds
> per-open SQLite auto-extension registration for optional adapters such as the
> gated sqlite-vec vector backend; default opens pass no extensions.

## Public Surface

```cpp
namespace orangutan::storage {

enum class OpenMode { read_only, read_write, read_write_create };

using SqliteExtensionInit = void (*)();

struct ConnectionOptions {
  std::string path;
  OpenMode mode{OpenMode::read_write_create};
  int busy_timeout_ms{5000};
  bool enable_wal{true};
  bool enforce_foreign_keys{true};
};

enum class StepResult { row, done };
using ColumnValue = std::optional<std::string>;
using BlobValue = std::optional<std::vector<std::byte>>;

struct Row {
  std::vector<ColumnValue> values;
};

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<Row> rows;
};

struct Migration {
  std::int64_t version;
  std::string name;
  std::string sql;
};

struct MigrationReport {
  std::int64_t previous_version;
  std::int64_t current_version;
  std::vector<std::int64_t> applied_versions;
};

class Connection {
 public:
  static core::Result<Connection> open(
      ConnectionOptions,
      std::span<const SqliteExtensionInit> auto_extensions = {});
  core::Result<void> execute(std::string_view sql);
  core::Result<Statement> prepare(std::string_view sql);
  core::Result<QueryResult> query(std::string_view sql);
  void close() noexcept;
};

class Statement {
 public:
  core::Result<void> bind_null(int index);
  core::Result<void> bind_int64(int index, std::int64_t value);
  core::Result<void> bind_double(int index, double value);
  core::Result<void> bind_text(int index, std::string_view value);
  core::Result<void> bind_blob(int index, std::span<const std::byte> value);
  core::Result<StepResult> step();
  core::Result<void> reset();
  core::Result<void> clear_bindings();
  core::Result<ColumnValue> column_text(int index) const;
  core::Result<BlobValue> column_blob(int index) const;
  core::Result<std::int64_t> column_int64(int index) const;
  core::Result<double> column_double(int index) const;
};

core::Result<MigrationReport> run_migrations(Connection&, std::span<const Migration>);
core::Result<std::vector<Migration>> load_migrations_from_directory(std::string_view directory);
core::Result<MigrationReport> run_migrations_from_directory(Connection&, std::string_view directory);

}  // namespace orangutan::storage
```

SQLite bind indices are 1-based. SQLite column indices are 0-based.
Column reader methods require the most recent `Statement::step()` result to be
`StepResult::row`; calling them before the first row, after `done`, or after `reset`
returns `core::ErrorKind::conflict`.

## Error Model

All public APIs return `core::Result<T>`. SQLite failures map to
`core::ErrorKind::storage` and include these context fields when available:

- `sqlite_code`
- `sqlite_extended_code`
- `sqlite_message`
- `sql`

Migration failures add `migration_version` and `migration_name` when the failure is
tied to a specific migration.

There are no throwing wrappers and no `must_ok` escape hatch.

## Lifetime

`Connection` and `Statement` are move-only. Internally, statements keep a shared
handle to the SQLite connection so a statement can finish/finalize even if the
`Connection` object that created it has been closed or moved away.

SQLite's auto-extension registry is process-global. `Connection::open` accepts an
optional span of extension init callbacks, serializes register/open/cancel under
an internal mutex, rejects null callbacks, and cancels temporary registrations
before returning. Callers use this only for feature-gated adapters; ordinary
storage users pass the default empty span.

## Defaults

- `busy_timeout_ms = 5000`
- `PRAGMA foreign_keys = ON`
- Verified `PRAGMA journal_mode = WAL` for file-backed read/write databases

Read-only connections do not try to switch journal mode. In-memory databases can opt
out of WAL with `enable_wal = false`.

## Migrations

`run_migrations(Connection&, std::span<const Migration>)` is synchronous and
connection-local. The pool layer calls it through the writer connection.

The runner creates this table in each database:

```sql
CREATE TABLE IF NOT EXISTS schema_versions(
  version INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  applied_at TEXT NOT NULL
);
```

The provided migration set must be complete and contiguous from version `1`. The runner
rejects non-positive versions, gaps, duplicates, empty names, and empty SQL before it
touches the database. Existing recorded versions must also be contiguous and must not be
newer than the provided set.

Each pending migration runs in its own `BEGIN IMMEDIATE` transaction. On success, the
runner inserts a `schema_versions` row with an ISO-like UTC timestamp from SQLite. On
failure, it rolls back that migration and returns `core::ErrorKind::storage` with
migration context. Re-running the same complete migration set is an idempotent no-op:
the report returns the existing `current_version` and an empty `applied_versions`.

SQL-file loading is a feeder path for the same runner:

```cpp
auto migrations = storage::load_migrations_from_directory(
    "src/oran-storage/migrations/sessions");
auto report = storage::run_migrations_from_directory(
    connection, "src/oran-storage/migrations/sessions");
```

Migration directories contain regular files named exactly `0001-name.sql`,
`0002-next-name.sql`, and so on. The four-digit prefix becomes
`Migration::version`, and the filename stem after the first dash becomes
`Migration::name`. The loader reads each file as binary text, sorts by version,
and reuses the same contiguous-list validation as `run_migrations` before any
database write occurs.

The loader rejects:

- Empty directory paths.
- Missing paths (`core::ErrorKind::not_found`).
- Non-directory paths.
- Empty migration directories.
- Regular files that do not match the `0001-name.sql` convention.
- Gaps, duplicate versions, non-positive versions, empty names, and empty SQL.
- Filesystem read/iteration failures (`core::ErrorKind::io` with `path`
  context).

## Future Slices

- Backup script integration and generated schema docs.
- Additional domain repositories for new schemas as memory, automation, and
  audit features grow, while keeping those schemas in their owning libraries.

## Pool

`Pool` opens one read/write/create writer connection plus N read-only reader
connections against the same database file. All readers and the writer share
SQLite's WAL journal so readers see committed writes without blocking the writer.
Acquisition is async and serialized through an `asio::any_io_executor` supplied
at construction.

```cpp
namespace orangutan::storage {

struct PoolOptions {
  std::string path;
  std::size_t reader_count{2};
  std::size_t statement_cache_capacity{32};
  int busy_timeout_ms{5000};
  bool enable_wal{true};
  bool enforce_foreign_keys{true};
};

class WriterLease {
 public:
  ~WriterLease();
  bool valid() const noexcept;
  Connection& connection() noexcept;
  StatementCache& statement_cache() noexcept;
  void release() noexcept;
};

class ReaderLease {
 public:
  ~ReaderLease();
  bool valid() const noexcept;
  std::size_t slot() const noexcept;
  Connection& connection() noexcept;
  StatementCache& statement_cache() noexcept;
  void release() noexcept;
};

class Pool {
 public:
  static core::Result<Pool> open(
      asio::any_io_executor,
      PoolOptions,
      std::span<const SqliteExtensionInit> auto_extensions = {});

  std::size_t reader_count() const noexcept;
  std::size_t readers_available() const noexcept;
  bool        writer_busy() const noexcept;

  async::Awaitable<core::Result<WriterLease>> acquire_writer();
  async::Awaitable<core::Result<ReaderLease>> acquire_reader();
};

}  // namespace orangutan::storage
```

When `auto_extensions` is supplied, `Pool::open` forwards the same temporary
extension list to the writer and every reader connection. This keeps extension
ownership at the caller boundary while preserving one pool-wide SQLite feature
set.

### Semantics

- The writer is exclusive. At most one `WriterLease` exists at a time.
  `acquire_writer` resolves immediately when the writer is free and otherwise
  queues a FIFO waiter that resumes when the previous lease releases.
- Readers are pooled. `acquire_reader` consumes a free slot, hands out a
  `ReaderLease` pinning a specific reader index, and resumes the next FIFO
  waiter on release. With `reader_count = N`, up to `N` reader leases can
  coexist.
- Readers open in `OpenMode::read_only`. Writes through a reader return
  `core::ErrorKind::storage` from SQLite.
- The writer opens in `OpenMode::read_write_create` with WAL when enabled.
  Migrations run through `acquire_writer()` followed by
  `run_migrations(lease.connection(), …)` or
  `run_migrations_from_directory(lease.connection(), …)`.
- `Pool::open` creates one `StatementCache` for the writer and one cache for
  each reader slot. The caches use `PoolOptions::statement_cache_capacity` and
  persist for the same lifetime as the slot connection, so a hot SQL string can
  hit after a lease releases and the same slot is acquired again.
- `WriterLease::statement_cache()` and `ReaderLease::statement_cache()` expose
  the slot cache next to `connection()`. Cached statement leases are
  lease-scoped work: callers should acquire/use/release `CachedStatement`
  objects while still holding the pool lease that owns the connection slot.
- Both lease types are move-only RAII. Destruction releases the slot back to
  the pool. `release()` is idempotent; once a lease has been released, `valid()`
  returns false and further `release()` calls are no-ops.
- Coroutine cancellation is honored. A waiter cancelled before its slot frees
  resolves with `core::ErrorKind::cancelled` and the slot remains available for
  the next waiter.
- The pool internals share state via `shared_ptr`. Leases keep that state alive,
  so a lease may outlive the `Pool` object that produced it (the underlying
  connections are closed when the last lease is destroyed).

### Error Model

- `Pool::open` returns `core::ErrorKind::invalid_argument` for an empty path,
  `reader_count = 0`, or `statement_cache_capacity = 0`. Per-connection open
  failures bubble up the SQLite error with `pool_role` (`writer` or `reader`)
  and `pool_slot` (reader index) context fields attached.
- Acquiring from a default-constructed (not-open) `Pool` returns
  `core::ErrorKind::conflict` with message `pool is not open`.

### Compile-Time Cost

The pool's public header pulls in `<asio/any_io_executor.hpp>` and
`<oran/async/awaitable_fwd.hpp>`, mirroring `oran-async`'s public surface.
It forward-declares `StatementCache`; callers that include the umbrella
`<oran/storage.hpp>` get the full cache definition. Storage TUs that consume
the pool inherit the asio include set; storage TUs that only touch
`Connection` / `Statement` remain unchanged. The xmake target graph lists
`oran-storage` as depending on `oran-async`.

## Statement Cache

`StatementCache` reuses prepared `Statement` objects keyed by SQL text so that
hot-path queries pay the `sqlite3_prepare_v2` cost once per cache lifetime
instead of once per call. Each cache is bound to a single `Connection` by
convention; SQLite statements are handle-specific, so sharing one cache across
multiple connections is undefined. `Pool` follows this rule by owning one cache
per writer / reader slot.

```cpp
namespace orangutan::storage {

struct StatementCacheOptions {
  std::size_t capacity{32};
};

class CachedStatement {
 public:
  ~CachedStatement();
  bool      valid() const noexcept;
  Statement& statement() noexcept;
  void      release() noexcept;
};

class StatementCache {
 public:
  static core::Result<StatementCache> open(StatementCacheOptions);

  bool         valid() const noexcept;
  std::size_t  capacity() const noexcept;
  std::size_t  size() const noexcept;
  std::size_t  hits() const noexcept;
  std::size_t  misses() const noexcept;
  std::size_t  evictions() const noexcept;

  core::Result<CachedStatement> acquire(Connection&, std::string_view sql);
  void                          clear() noexcept;
};

}  // namespace orangutan::storage
```

### Semantics

- `acquire` returns a move-only RAII `CachedStatement` lease.
  - Cache hit: the existing entry is promoted to most-recently-used, the
    statement is `sqlite3_reset`/`sqlite3_clear_bindings`-ed before hand-out,
    and `hits` increments.
  - Cache miss: the SQL is prepared via `Connection::prepare`, the new entry is
    inserted at the front of the LRU, and `misses` increments. If the cache is
    at capacity, the least-recently-used unleased entry is evicted and
    `evictions` increments.
- If every entry is currently leased on a miss, the new statement is **transient**
  — it is not inserted into the cache, has no eviction effect on existing
  entries, and is finalized when the lease releases. This keeps the cache size
  bounded without forcing callers to handle a "cache is busy" error.
- `acquire`-ing the same SQL while a prior lease is still outstanding returns
  `core::ErrorKind::conflict` (the underlying statement is single-handed).
- `CachedStatement::~CachedStatement` resets + clears bindings on the statement
  and returns it to the cache (or finalizes it, if the entry was orphaned by
  `clear` / eviction). `release()` is idempotent.
- `clear()` purges every unleased entry, marks leased entries orphaned (so they
  finalize on release rather than re-enter the cache), and resets the counters.

### Error Model

- `StatementCache::open` returns `core::ErrorKind::invalid_argument` when
  `capacity == 0`.
- Calling `acquire` on a default-constructed (not-open) cache returns
  `core::ErrorKind::conflict` with message `statement cache is not open`.
- An empty SQL string returns `core::ErrorKind::invalid_argument`.
- Per-statement failures bubble the SQLite error from `Connection::prepare`,
  `Statement::reset`, or `Statement::clear_bindings`, with an `sql` context
  field attached on the reset/clear path.

### Compile-Time Cost

The cache's public header pulls in stdlib (`<cstddef>`, `<memory>`,
`<string_view>`) plus `<oran/core/result.hpp>` and `<oran/storage/sqlite.hpp>` —
all already on the storage public surface. The implementation adds `<list>` and
`<unordered_map>`, both confined to `src/oran-storage/statement_cache.cpp`. No
new public dependency edge.

### Threading

A cache is single-owner, matching `Connection`'s `SQLITE_OPEN_NOMUTEX` mode.
Concurrent use from multiple threads is undefined. `Pool` owns one cache per
writer / reader slot so single-owner discipline is preserved at the lease
boundary.

## Session Repository

`SessionRepository` is the first storage domain repository. It stores session
message rows and per-session skill activation rows in `sessions.db` using the
cached pool surface. It is deliberately payload-oriented: `content_json` and
`metadata_json` are opaque strings at this layer; slice 130's
`oran-memory::session::Store` owns typed `core::Message` serialization above it,
and slice 148's memory wrapper owns the semantic skill activation update/record
shapes above the raw table. `role` is typed at the API boundary: requests take
and records expose `core::Role`, and the row's text column is parsed back into
the enum on read (rows with unknown role text surface a storage error rather than
being silently coerced).

```cpp
namespace orangutan::storage {

struct SessionKey {
  std::string session_id;
  std::string agent_key;
};

struct AppendSessionMessageRequest {
  std::string session_id;
  std::string agent_key;
  core::Role  role{core::Role::user};
  std::string content_json;
  std::string metadata_json{"{}"};
};

struct SessionMessageRecord {
  std::string  session_id;
  std::string  agent_key;
  std::int64_t sequence{};
  core::Role   role{core::Role::user};
  std::string  content_json;
  std::string  metadata_json;
  std::string  created_at;
};

struct UpsertSessionSkillActivationRequest {
  std::string session_id;
  std::string agent_key;
  std::string skill_name;
  bool active;
};

struct SessionSkillActivationRecord {
  std::string session_id;
  std::string agent_key;
  std::string skill_name;
  bool active;
  std::string created_at;
  std::string updated_at;
};

struct SessionRecord {
  std::string                session_id;
  std::string                agent_key;
  std::optional<std::string> title;
  std::string                metadata_json;
  std::string                created_at;
  std::string                updated_at;
  std::int64_t               message_count{};
};

struct ListSessionsOptions {
  std::string agent_key;
  std::size_t limit{50};
};

struct SessionRepositoryOptions {
  std::string migrations_directory;
};

class SessionRepository {
 public:
  explicit SessionRepository(Pool&, SessionRepositoryOptions = {}) noexcept;

  async::Awaitable<core::Result<MigrationReport>> migrate();
  async::Awaitable<core::Result<SessionMessageRecord>>
  append_message(AppendSessionMessageRequest);
  async::Awaitable<core::Result<std::vector<SessionMessageRecord>>>
  load_messages(SessionKey);
  async::Awaitable<core::Result<SessionSkillActivationRecord>>
  upsert_skill_activation(UpsertSessionSkillActivationRequest);
  async::Awaitable<core::Result<std::vector<SessionSkillActivationRecord>>>
  load_skill_activations(SessionKey);
  async::Awaitable<core::Result<std::optional<SessionRecord>>>
  get_session(SessionKey);
  async::Awaitable<core::Result<std::vector<SessionRecord>>>
  list_sessions(ListSessionsOptions);
};

}  // namespace orangutan::storage
```

### Schema

Migration `1 / sessions-initial` lives at
`src/oran-storage/migrations/sessions/0001-sessions-initial.sql` and creates:

- `sessions(session_id, agent_key, title, metadata_json, created_at,
  updated_at)`, primary-keyed by `(session_id, agent_key)`.
- `session_messages(session_id, agent_key, sequence, role, content_json,
  metadata_json, created_at)`, primary-keyed by `(session_id, agent_key,
  sequence)`.
- `trg_session_messages_touch_session`, an insert trigger that creates or
  touches the owning `sessions` row after every message append.

Migration `2 / session-skill-activations` lives at
`src/oran-storage/migrations/sessions/0002-session-skill-activations.sql` and
creates `session_skill_activations(session_id, agent_key, skill_name, active,
created_at, updated_at)`, primary-keyed by `(session_id, agent_key, skill_name)`,
plus an index on `(session_id, agent_key, active, skill_name)`. The row stores
the latest activation decision for one skill in one session/agent scope; `active`
is constrained to `0` or `1`.

`append_message` is the hot path. It acquires a writer lease, uses
`lease.statement_cache()` for the insert SQL, computes `sequence` as
`MAX(sequence)+1` for the `(session_id, agent_key)` pair, and returns the stored
sequence + timestamp. `upsert_skill_activation` uses a writer lease and SQLite
UPSERT to replace only the latest active/inactive decision for a skill while
preserving the original `created_at`; `load_skill_activations`, `load_messages`,
`get_session`, and `list_sessions` acquire reader leases and use the reader
slot's statement cache.

`migrate()` acquires a writer lease and calls
`run_migrations_from_directory`. With default options it looks for
`src/oran-storage/migrations/sessions` from the process current directory and
then each parent directory, so both repo-root and xmake build-dir runs find the
checked-in source migration. `SessionRepositoryOptions::migrations_directory`
overrides that lookup for future bootstrap/install packaging.

### Error Model

Empty `session_id`, `agent_key`, `skill_name`, `role`, `content_json`, and
`metadata_json` return `core::ErrorKind::invalid_argument` before touching SQLite.
SQLite failures bubble through the existing storage error context. Migration file
lookup failures return the same `load_migrations_from_directory` errors, with
`not_found`, `invalid_argument`, or `io` kinds depending on the failure.

### Compile-Time Cost

The public header uses stdlib containers/strings plus existing
`oran-async`/`oran-core`/`oran-storage` forward surfaces. The implementation
keeps hot SQL strings, row mappers, source-tree migration lookup, and
statement-cache usage in `src/oran-storage/session_repository.cpp`; the schema
DDL itself lives in the migration file.

## Audit Repository

`AuditRepository` is the storage-backed half of the permission audit pipeline.
It stores permission decisions and hook-publish observability rows in
`audit.db`, partitioned by `scope_key` and searchable by `agent_key`,
`tool_name`, `event_kind`, and `outcome`. The permission layer owns the enum
vocabulary and sink abstraction; storage owns the SQLite schema and typed
repository operations.

```cpp
namespace orangutan::storage {

struct AppendAuditEventRequest {
  std::string event_kind{"permission_decision"};
  std::string scope_key;
  std::string agent_key;
  std::string tool_name;
  std::string identity;
  std::string verdict;
  std::string outcome;
  std::string reason;
  std::string input_hash_hex{};
  std::optional<core::TurnId> parent_turn_id{};
  std::string metadata_json{"{}"};
};

struct UpdateAuditEventMetadataRequest {
  std::string event_kind{"permission_decision"};
  std::string scope_key;
  std::string agent_key;
  std::string tool_name;
  std::string identity;
  std::string input_hash_hex{};
  std::optional<core::TurnId> parent_turn_id{};
  std::string previous_metadata_json{"{}"};
  std::string metadata_json{"{}"};
};

class AuditRepository {
 public:
  explicit AuditRepository(Pool&, AuditRepositoryOptions = {}) noexcept;

  async::Awaitable<core::Result<MigrationReport>> migrate();
  async::Awaitable<core::Result<AuditEventRecord>>
  append_event(AppendAuditEventRequest);
  async::Awaitable<core::Result<AuditEventRecord>>
  update_event_metadata(UpdateAuditEventMetadataRequest);
  async::Awaitable<core::Result<std::vector<AuditEventRecord>>>
  list_events(ListAuditEventsOptions);
  async::Awaitable<core::Result<std::vector<AuditEventRecord>>>
  list_events_for_turn(core::TurnId parent_turn_id, std::size_t limit = 200);
  async::Awaitable<core::Result<std::int64_t>>
  count_events(std::string scope_key);
};

}  // namespace orangutan::storage
```

Slice 67 adds `update_event_metadata` for post-result audit enrichment. Slice
79 adds optional `parent_turn_id` to append records, listed records, and
metadata updates. Slice 93 adds `event_kind` to append/update/list records and
to the update match key, so enrichment for ordinary permission rows cannot
clobber a same-tool `hook_publish` row. The update matches by the same event
identity fields as the append path (`event_kind`, `scope_key`, `agent_key`,
`tool_name`, `identity`, optional input hash, optional parent turn id) plus the
previously stored metadata JSON, then updates the newest matching row. This lets
`tool::Registry::dispatch` record the permission decision before the handler runs and later add
`metadata_json.usage` after a successful, capped tool result without weakening
the durable decision audit or clobbering a same-tool call from another turn.

### Schema

Migration `1 / audit-initial` lives at
`src/oran-storage/migrations/audit/0001-audit-initial.sql` and creates
`audit_events(id, scope_key, agent_key, tool_name, identity, verdict, outcome,
reason, input_hash_hex, metadata_json, created_at)` plus scope/time,
agent/time, and outcome/time indexes. `metadata_json` is intentionally opaque to
SQLite v1; callers own the JSON shape and storage validates only that it is
non-empty.

The default `AuditRepository::migrate()` path now applies the complete audit DB
migration stream: version 1 creates `audit_events`, and version 2 adds
`trace_turns` for spec 0018. Version 3 adds nullable
`audit_events.parent_turn_id` as a 16-byte BLOB plus
`idx_audit_events_parent_turn` so trace inspectors can join a turn row to its
tool audit rows without scanning the whole audit table. Version 4 adds
`audit_events.event_kind TEXT NOT NULL DEFAULT 'permission_decision'` plus the
`idx_audit_events_kind_parent_turn` partial index so hook-publish rows can share
the same parent-turn cause chain without changing existing permission-decision
payloads. Explicit
`AuditRepositoryOptions::migrations_directory` still supplies a caller-owned
migration set for tests and packaged layouts.

### Error Model

Empty required append/update fields and zero-valued parent turn ids return
`core::ErrorKind::invalid_argument` before touching SQLite. An update whose key
and previous metadata do not match an existing row returns
`core::ErrorKind::not_found`, which callers may treat as best-effort enrichment
failure while preserving the original decision row.

## Trace Repository

`TraceRepository` is the storage-owned foundation for spec 0018's first-loop
observability row. It stores one redacted row per agent turn in `trace_turns`.
The row is body-free: prompt bytes, tool inputs, memory facts, and provider
responses stay out of the trace table; only ids, hashes, byte counts, token
counts, route labels, stop/cancellation classifications, and opaque context
bytes are persisted.

```cpp
namespace orangutan::storage {

using TraceId = core::TurnId;

struct AppendTraceTurnRequest {
  TraceId turn_id;
  std::optional<TraceId> parent_turn_id;
  TraceId session_id;
  std::string agent_key;
  std::string origin;
  std::string route_profile;
  std::string route_model;
  std::int64_t started_at_ns;
  std::int64_t finished_at_ns;
  std::string stop_reason;
  std::int64_t iteration_count{1};
  std::uint64_t prompt_prefix_hash;
  std::int64_t prompt_prefix_bytes;
  std::uint64_t active_catalog_hash;
  std::uint64_t deferred_catalog_hash;
  std::int64_t cache_creation_tokens{};
  std::int64_t cache_read_tokens{};
  std::int64_t input_tokens{};
  std::int64_t output_tokens{};
  double cost_estimate_usd{};
  std::optional<std::string> cancellation_phase;
  std::string context_json{"{}"};
  std::int64_t schema_version{1};
};

class TraceRepository {
 public:
  explicit TraceRepository(Pool&, TraceRepositoryOptions = {}) noexcept;

  async::Awaitable<core::Result<MigrationReport>> migrate();
  async::Awaitable<core::Result<TraceTurnRecord>>
  append_turn(AppendTraceTurnRequest);
  async::Awaitable<core::Result<std::optional<TraceTurnRecord>>>
  get_turn(TraceId turn_id);
  async::Awaitable<core::Result<std::vector<TraceTurnRecord>>>
  list_turns(ListTraceTurnsOptions);
  async::Awaitable<core::Result<std::vector<ProviderUsageRollup>>>
  list_provider_usage_rollups(ListProviderUsageRollupsOptions);
  async::Awaitable<core::Result<std::int64_t>>
  purge_turns_started_before(std::int64_t started_before_ns);
  async::Awaitable<core::Result<std::int64_t>> count_turns();
};

}  // namespace orangutan::storage
```

Migration `2 / trace-turns-initial` lives at
`src/oran-storage/migrations/audit/0002-trace-turns-initial.sql` and creates
`trace_turns(turn_id, parent_turn_id, session_id, agent_key, origin,
route_profile, route_model, started_at_ns, finished_at_ns, stop_reason,
iteration_count, prompt_prefix_hash, prompt_prefix_bytes, active_catalog_hash,
deferred_catalog_hash, cache_creation_tokens, cache_read_tokens, input_tokens,
output_tokens, cost_estimate_usd, cancellation_phase, context_json,
schema_version)` plus session/time and agent/time indexes. `turn_id`,
`parent_turn_id`, and `session_id` are 16-byte BLOB values; `context_json` is
stored as BLOB bytes and defaults to `{}`.

The repository validates non-zero ids, non-empty required text fields,
positive `iteration_count` / `schema_version`, non-negative counters, and
`finished_at_ns >= started_at_ns` before touching SQLite. It does not parse
`context_json`; the agent/trace writer owns redaction and JSON shape.
`list_provider_usage_rollups` is a read-only derived query over the same
`trace_turns` rows. It groups by `strftime('%Y-%m-%d', started_at_ns, 'unixepoch')`,
`agent_key`, `route_profile`, and `route_model`, returns newest days first, and
supports optional agent/profile/model filters plus a positive `limit`.
`purge_turns_started_before` deletes only `trace_turns` rows whose
`started_at_ns` is strictly older than an explicit Unix-nanosecond cutoff and
returns the number deleted. It validates the cutoff as non-negative, acquires a
writer lease, and does not touch `audit_events`; audit retention remains a
separate policy.

Slice 78 deliberately stopped at the storage primitive. Slice 79 threads a typed
turn id through `agent::Loop`, `tool::DispatchContext`, and the permission audit
sink so tool audit rows can join against `trace_turns` rows. Slice 80 adds the
first consumer: `agent::Loop` appends one row for terminal-success turns when
callers supply `RunTurnInputs::trace.repository` and a turn id. Slice 85 lets the
loop generate that turn id when a trace writer is configured and callers leave
it unset. Slices 86-93 add iteration-cap rows, trace config/runtime wiring, the
CLI inspector, and hook-publish audit rows. Slice 127 adds the first
trace-derived provider usage rollup read, slice 129 adds profile-priced cost
calculation before trace rows are written, and slice 150 adds explicit-cutoff
trace row retention.

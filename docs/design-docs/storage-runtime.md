# Storage Runtime

`oran-storage` is the expected-only SQLite foundation used by sessions, memory,
automation, audit logs, and configuration metadata. It owns the SQLite dependency and
does not expose `sqlite3.h` from public headers.

> **Storage status (2026-05-24):** `oran-storage` ships `Connection`, `Statement`,
> typed binding/stepping/column readers, WAL + foreign-key setup, a simple `query`
> helper, the synchronous `run_migrations` runner plus SQL-file migration
> loading, compile-time embedded audit/session migrations via C++26 `#embed`,
> an async writer/reader `Pool` driven by `oran-async` executors with one
> `StatementCache` per writer or reader slot, and the standalone per-connection
> `StatementCache` with LRU eviction. `SessionRepository` persists opaque
> session-message JSON through the cached pool surface (with `role` typed as
> `core::Role` at the API boundary). `AuditRepository` persists permission
> decision rows in `audit_events` with append/list/count plus slice-67
> `update_event_metadata` so post-result usage metadata can enrich the same
> audit row without appending a second decision. Backups and the memory /
> automation repositories are future slices.

## Public Surface

```cpp
namespace orangutan::storage {

enum class OpenMode { read_only, read_write, read_write_create };

struct ConnectionOptions {
  std::string path;
  OpenMode mode{OpenMode::read_write_create};
  int busy_timeout_ms{5000};
  bool enable_wal{true};
  bool enforce_foreign_keys{true};
};

enum class StepResult { row, done };
using ColumnValue = std::optional<std::string>;

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
  static core::Result<Connection> open(ConnectionOptions);
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
  core::Result<StepResult> step();
  core::Result<void> reset();
  core::Result<void> clear_bindings();
  core::Result<ColumnValue> column_text(int index) const;
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

- Migration asset packaging for installed/runtime layouts outside a source
  checkout.
- Domain repositories for memory, automation, and audit logs.
- Backup script integration and generated schema docs.

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
  static core::Result<Pool> open(asio::any_io_executor, PoolOptions);

  std::size_t reader_count() const noexcept;
  std::size_t readers_available() const noexcept;
  bool        writer_busy() const noexcept;

  async::Awaitable<core::Result<WriterLease>> acquire_writer();
  async::Awaitable<core::Result<ReaderLease>> acquire_reader();
};

}  // namespace orangutan::storage
```

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
message rows in `sessions.db` using the cached pool surface. It is deliberately
payload-oriented: `content_json` and `metadata_json` are opaque strings at this
layer, and the future `oran-memory::session::Store` will own typed
`core::Message` serialization. `role` is typed at the API boundary: requests
take and records expose `core::Role`, and the row's text column is parsed back
into the enum on read (rows with unknown role text surface a storage error
rather than being silently coerced).

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

`append_message` is the hot path. It acquires a writer lease, uses
`lease.statement_cache()` for the insert SQL, computes `sequence` as
`MAX(sequence)+1` for the `(session_id, agent_key)` pair, and returns the stored
sequence + timestamp. `load_messages`, `get_session`, and `list_sessions`
acquire reader leases and use the reader slot's statement cache.

`migrate()` acquires a writer lease and calls
`run_migrations_from_directory`. With default options it looks for
`src/oran-storage/migrations/sessions` from the process current directory and
then each parent directory, so both repo-root and xmake build-dir runs find the
checked-in source migration. `SessionRepositoryOptions::migrations_directory`
overrides that lookup for future bootstrap/install packaging.

### Error Model

Empty `session_id`, `agent_key`, `role`, `content_json`, and `metadata_json`
return `core::ErrorKind::invalid_argument` before touching SQLite. SQLite
failures bubble through the existing storage error context. Migration file
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
It stores one row per permission decision in `audit.db`, partitioned by
`scope_key` and searchable by `agent_key`, `tool_name`, and `outcome`. The
permission layer owns the enum vocabulary and sink abstraction; storage owns the
SQLite schema and typed repository operations.

```cpp
namespace orangutan::storage {

struct AppendAuditEventRequest {
  std::string scope_key;
  std::string agent_key;
  std::string tool_name;
  std::string identity;
  std::string verdict;
  std::string outcome;
  std::string reason;
  std::string input_hash_hex{};
  std::string metadata_json{"{}"};
};

struct UpdateAuditEventMetadataRequest {
  std::string scope_key;
  std::string agent_key;
  std::string tool_name;
  std::string identity;
  std::string input_hash_hex{};
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
  async::Awaitable<core::Result<std::int64_t>>
  count_events(std::string scope_key);
};

}  // namespace orangutan::storage
```

Slice 67 adds `update_event_metadata` for post-result audit enrichment. The
update matches by the same event identity fields as the append path
(`scope_key`, `agent_key`, `tool_name`, `identity`, optional input hash) plus
the previously stored metadata JSON, then updates the newest matching row. This
lets `tool::Registry::dispatch` record the permission decision before the
handler runs and later add `metadata_json.usage` after a successful, capped tool
result without weakening the durable decision audit.

### Schema

Migration `1 / audit-initial` lives at
`src/oran-storage/migrations/audit/0001-audit-initial.sql` and creates
`audit_events(id, scope_key, agent_key, tool_name, identity, verdict, outcome,
reason, input_hash_hex, metadata_json, created_at)` plus scope/time,
agent/time, and outcome/time indexes. `metadata_json` is intentionally opaque to
SQLite v1; callers own the JSON shape and storage validates only that it is
non-empty.

### Error Model

Empty required append/update fields return `core::ErrorKind::invalid_argument`
before touching SQLite. An update whose key + previous metadata do not match an
existing row returns `core::ErrorKind::not_found`, which callers may treat as
best-effort enrichment failure while preserving the original decision row.

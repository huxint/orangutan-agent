# Storage Runtime

`oran-storage` is the expected-only SQLite foundation used by sessions, memory,
automation, audit logs, and configuration metadata. It owns the SQLite dependency and
does not expose `sqlite3.h` from public headers.

> **Storage status (2026-05-16):** `oran-storage` ships `Connection`, `Statement`,
> typed binding/stepping/column readers, WAL + foreign-key setup, a simple `query`
> helper, the synchronous `run_migrations` runner, an async writer/reader
> `Pool` driven by `oran-async` executors with one `StatementCache` per writer
> or reader slot, and the standalone per-connection `StatementCache` with LRU
> eviction. SQL-file loading, backups, and domain repositories are future
> slices.

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
connection-local. The future pool layer will call it through the writer connection.

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

## Future Slices

- SQL-file loading from `src/oran-storage/migrations/`.
- Domain repositories for sessions, memory, automation, and audit logs.
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
  `run_migrations(lease.connection(), …)`.
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
Concurrent use from multiple threads is undefined. Future pool integration will
give each pool slot its own cache so single-owner discipline is preserved.

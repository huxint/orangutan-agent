# Storage Runtime

`oran-storage` is the expected-only SQLite foundation used by sessions, memory,
automation, audit logs, and configuration metadata. It owns the SQLite dependency and
does not expose `sqlite3.h` from public headers.

> **Storage status (2026-05-15):** `oran-storage` ships `Connection`, `Statement`,
> typed binding/stepping/column readers, WAL + foreign-key setup, a simple `query`
> helper, and the synchronous `run_migrations` runner. Pools, statement caches,
> backups, SQL-file loading, and domain repositories are future slices.

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

- `Pool` with one writer connection on a strand and reader connections round-robin.
- SQL-file loading from `src/oran-storage/migrations/`.
- Prepared statement cache.
- Domain repositories for sessions, memory, automation, and audit logs.
- Backup script integration and generated schema docs.

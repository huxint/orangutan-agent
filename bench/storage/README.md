# `bench/storage/` — nanobench scenarios for `oran-storage`

## What this bucket benchmarks

`oran-storage` is the expected-only SQLite core used by sessions, memory, automation,
audit, and trace repositories. The scenarios measure insert-path tradeoffs,
compiled and SQL-file migration startup cost, statement-cache reuse, the
per-query overhead of the async writer/reader pool, and the first domain
repository wrappers.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/migrations.cpp`](scenarios/migrations.cpp) | Compiled-span cold/no-op migration runs *vs.* SQL-file load+run cold/no-op migration runs. |
| [`scenarios/sqlite_insert.cpp`](scenarios/sqlite_insert.cpp) | Literal `Connection::execute` inserts *vs.* prepared `Statement` binding. |
| [`scenarios/pool_acquire.cpp`](scenarios/pool_acquire.cpp) | Direct `Connection` re-use *vs.* `Pool::acquire_reader` + `query` for the same SELECT batch. |
| [`scenarios/statement_cache.cpp`](scenarios/statement_cache.cpp) | Fresh prepare *vs.* standalone `StatementCache` prepare reuse. |
| [`scenarios/pool_statement_cache.cpp`](scenarios/pool_statement_cache.cpp) | Pool writer fresh prepare *vs.* pool writer slot `StatementCache` reuse. |
| [`scenarios/session_repository.cpp`](scenarios/session_repository.cpp) | Raw pool + cache SQL append/load *vs.* `SessionRepository` append/load. |
| [`scenarios/audit_repository.cpp`](scenarios/audit_repository.cpp) | Raw pool + cache SQL append/count *vs.* `AuditRepository` append/list for a 64-event batch. |
| [`scenarios/trace_repository.cpp`](scenarios/trace_repository.cpp) | Raw pool + cache SQL trace insert *vs.* `TraceRepository` insert for a 32-turn batch. |

## Running

```sh
xmake build bench-storage
xmake run bench-storage
```

Output is nanobench's markdown shape on stdout. Stable baseline JSON is still a
future benchmark-harness task.

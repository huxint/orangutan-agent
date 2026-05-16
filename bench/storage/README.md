# `bench/storage/` — nanobench scenarios for `oran-storage`

## What this bucket benchmarks

`oran-storage` is the expected-only SQLite core used by sessions, memory, automation,
and audit repositories. The scenarios measure insert-path tradeoffs, migration
startup cost, and the per-query overhead of the async writer/reader pool.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/migrations.cpp`](scenarios/migrations.cpp) | Cold migration apply *vs.* no-op migration check. |
| [`scenarios/sqlite_insert.cpp`](scenarios/sqlite_insert.cpp) | Literal `Connection::execute` inserts *vs.* prepared `Statement` binding. |
| [`scenarios/pool_acquire.cpp`](scenarios/pool_acquire.cpp) | Direct `Connection` re-use *vs.* `Pool::acquire_reader` + `query` for the same SELECT batch. |

## Running

```sh
xmake build bench-storage
xmake run bench-storage
```

Output is nanobench's markdown shape on stdout. Stable baseline JSON is still a
future benchmark-harness task.

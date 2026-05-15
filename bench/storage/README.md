# `bench/storage/` — nanobench scenarios for `oran-storage`

## What this bucket benchmarks

`oran-storage` is the expected-only SQLite core used by sessions, memory, automation,
and audit repositories. The first scenario measures the cost difference between
literal `execute` inserts and a reused prepared statement with bound parameters.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/sqlite_insert.cpp`](scenarios/sqlite_insert.cpp) | Literal `Connection::execute` inserts *vs.* prepared `Statement` binding. |

## Running

```sh
xmake build bench-storage
xmake run bench-storage
```

Output is nanobench's markdown shape on stdout. Stable baseline JSON is still a
future benchmark-harness task.

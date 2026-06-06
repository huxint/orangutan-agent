# `bench/` — Benchmark Buckets

`bench/` is **first-class** alongside `tests/`. Every library has both.

## Layout

```
bench/
├── bench-helpers/          # shared corpus generators, mock IO, etc.
├── core/
├── async/
├── http/
├── storage/
├── config/
├── cli/
├── bootstrap/
├── permission/
├── tool/
├── memory/
├── skill/
├── provider/
├── prompt/
├── agent/
├── orchestration/
├── automation/
├── channel/
├── compile-time/           # per-TU compile-time scenarios
└── README.md               # this file
```

## Conventions

- nanobench is the default runner; Catch2's `BENCHMARK` macro is also accepted.
- Each bucket has:
  - `bench/<lib>/main.cpp` — entry point.
  - `bench/<lib>/scenarios/<scenario>.cpp` — one file per scenario.
  - `bench/<lib>/README.md` — describes what scenarios exist and what they compare.
- Each meaningful design tradeoff ships an **A-vs-B** comparison.
- Output: machine-readable JSON to stdout when `--json` flag is set.
- The live buckets include `bench/skill/` for the section-4 skill catalog
  renderer and owner, and `bench/automation/` for deterministic periodic
  schedule and memory-retention planning.
- See [`../docs/rules/testing-and-bench.md`](../docs/rules/testing-and-bench.md) and
  [`../docs/product-specs/0010-benchmark-harness.md`](../docs/product-specs/0010-benchmark-harness.md).

## Running

```sh
# Build one bucket and run
xmake build bench-memory && xmake run bench-memory --json > out.json

# Compare against baseline
scripts/bench-compare.sh memory

# Build the unified bench binary
xmake build orangutan-bench
xmake run orangutan-bench --json > all.json
```

## A-vs-B Spotlight

| Bucket             | A                              | B                          |
| ------------------ | ------------------------------ | -------------------------- |
| `core`             | variant-based `Content`        | polymorphic `Content` (stretch) |
| `async`            | direct coroutine post loop     | bounded `Channel<T>` handoff |
| `http`             | client construction            | invalid request validation |
| `io`               | direct `std::ifstream` read    | coroutine `read_text_file` wrapper |
| `storage`          | literal `execute` inserts / compiled-span migration apply / direct `Connection` read / fresh prepare / raw pool session SQL | prepared statement binding / SQL-file migration load+apply / `Pool::acquire_reader` read / cached prepare / `SessionRepository` |
| `config`           | in-memory JSON parse           | checked-in config file load |
| `cli`              | single-shot prompt dispatch    | empty REPL shell dispatch |
| `bootstrap`        | missing default config fallback | explicit config file load |
| `skill`            | order-trusting metadata concat | deterministic catalog render |
| `prompt`           | default active-tool set        | explicit active-tool subset |
| `agent`            | no promoted tools              | after `tool.search` promotion |
| `memory`           | raw `SessionRepository` append/load | typed `memory::session::Store` append/load |
| `provider`         | cache hints enabled            | cache hints disabled       |
| `orchestration`    | leader-worker strategy         | vote strategy              |
| `automation`       | periodic schedule evaluation   | memory-retention planning  |
| `tool`             | hashmap registry lookup        | static dispatch (stretch)  |
| `channel`          | bounded `Channel<T>`           | unbounded queue            |

Each table row corresponds to at least one scenario file in the bucket.

## Status

`bench/core/`, `bench/async/`, `bench/http/`, `bench/io/`, `bench/storage/`, `bench/config/`,
`bench/permission/`, `bench/hook/`, `bench/memory/`, `bench/automation/`, `bench/skill/`, `bench/tool/`,
`bench/prompt/`, `bench/provider/`, `bench/agent/`, `bench/cli/`, and
`bench/bootstrap/` are live.
Additional buckets land with their owning libraries.

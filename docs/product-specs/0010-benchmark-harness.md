# 0010 — Benchmark Harness

## User Problem

Performance contracts go un-maintained when there's nowhere to record what "good"
looks like. v2 ships `bench/<lib>/` for every library, runs them in CI, tracks them
over time, and uses **A-vs-B comparisons** to keep design tradeoffs honest.

## Scope (v1)

- `bench/<lib>/` directory per library; at least one scenario per library.
- `bench-helpers/` library with shared utilities (corpus generators, mock
  channels, fake providers).
- `orangutan-bench` binary that links every `bench-<lib>` and produces a unified JSON
  output.
- `scripts/bench-compare.sh` — runs a library's bench, compares to baseline, prints
  table, exits non-zero on > 10% regression.
- Baseline files committed under `docs/generated/bench-baseline-<lib>.json`.
- Nightly CI job runs the full bench suite, uploads JSON, opens a PR to update
  baselines if all green.

## Scope (v1.1)

- Compile-time bench under `bench/compile-time/` — measures the project's clean and
  incremental build times against the budget.
- Comparison runner that overlays multiple JSON outputs as a Markdown table
  suitable for PR comments.

## Scope (v2)

- Continuous performance dashboard (static HTML) generated nightly from the JSON
  history.
- Cross-machine normalization (record CPU + RAM + storage; warn when comparing
  across machines).

## Out Of Scope

- Performance regression detection on per-PR basis (only available for PRs labeled
  `perf-impact`).
- Real-network benches in CI (gated by env var).

## A-vs-B Convention

Every library that has a design tradeoff with plausible alternatives ships at least
one A-vs-B scenario:

| Library         | A                              | B                          |
| --------------- | ------------------------------ | -------------------------- |
| `oran-core`     | variant-based Content          | polymorphic Content (stretch) |
| `oran-async`    | direct coroutine post loop     | bounded `Channel<T>` handoff |
| `oran-io`       | direct `std::ifstream` text read | coroutine `read_text_file` wrapper |
| `oran-storage`  | literal `Connection::execute` inserts / cold migration apply | prepared `Statement` binding / no-op migration check |
| `oran-config`   | in-memory JSON parse             | checked-in config file load |
| `oran-cli`      | single-shot prompt dispatch      | empty REPL shell dispatch |
| `oran-bootstrap` | missing default config fallback | explicit config file load |
| `oran-skill`    | order-trusting metadata concat   | deterministic catalog render |
| `oran-tool`     | hashmap lookup                 | static dispatch (stretch)  |
| `oran-memory`   | raw `SessionRepository` append/load | typed `memory::session::Store` append/load; FTS5 vs sqlite-vec returns with long-term memory |
| `oran-provider` | cache hints enabled            | cache hints disabled       |
| `oran-orchestration` | leader-worker             | vote                       |
| `oran-automation` | periodic schedule evaluation | memory-retention request planning |
| `oran-channel`  | bounded asio channel           | unbounded queue (with backpressure metric) |

## Acceptance Criteria

1. `xmake build bench-<lib>` succeeds for every library that has a `bench/<lib>/`
   directory.
2. Running every bench produces a single JSON file ≤ 1 MB.
3. The comparison runner detects a synthetic 25% regression and exits non-zero.
4. Baselines update via PR (not auto-commit on `main`).
5. The nightly job runtime is ≤ 30 minutes.

## Design Doc Cross-References

- [`../rules/testing-and-bench.md`](../rules/testing-and-bench.md)
- [`../rules/compile-budget.md`](../rules/compile-budget.md)
- [`../FAST_COMPILATION.md`](../FAST_COMPILATION.md)

## Validation

```sh
xmake build bench-memory bench-async bench-provider
xmake run bench-memory --json > bench-memory.json
scripts/bench-compare.sh memory
```

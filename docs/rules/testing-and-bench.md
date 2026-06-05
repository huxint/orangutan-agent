# Testing And Benchmarking

Tests and benchmarks are **first-class peers**. Every library has both. This file
documents the layout, frameworks, and expectations.

## Layout

```
tests/
├── core/                 Catch2 bucket for oran-core
├── async/                Catch2 bucket for oran-async
├── log/
├── io/
├── http/
├── storage/
├── config/
├── cli/
├── bootstrap/
├── permission/
├── skill/
├── hook/
├── tool/
├── memory/
├── provider/
├── prompt/
├── agent/
├── orchestration/
├── automation/
├── channel/
├── channel-qq/           (optional, gated by channel_qq)
├── desktop/
├── integration/          end-to-end tests
└── test-helpers/         shared helpers: unique paths, fake providers, mock channels
```

```
bench/
├── core/                 nanobench/Catch2 bucket
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
├── compile-time/         per-TU compile-time measurements
└── bench-helpers/        shared helpers
```

## Tests

### Framework: Catch2 v3

- Tag every test with a category: `[unit]`, `[integration]`, `[property]`.
- Test case names are imperative: `TEST_CASE("loop completes a single-iteration turn",
  "[unit]")`.
- Use `SECTION` for sub-scenarios that share setup.

### Test Helpers (`tests/test-helpers/`)

Carry over from legacy:

- `unique_test_path()` — gives a temp path namespaced by test name.
- `unique_test_db_path()` — for SQLite tests.
- `ScopedEnvVar` — RAII env var.
- `FakeProviderBackend` — captures requests, returns canned responses.
- `make_test_route(...)` — builds a provider Route for tests.
- `mock_channel::*` — minimal channel adapter for orchestration tests.

### Coverage Expectations

| Library category | Min coverage |
| --- | --- |
| Foundation (core, async, log, io)         | 90% |
| Storage / config / permission             | 85% |
| Tool / memory / hook / skill              | 80% |
| Provider / prompt / agent                  | 75% |
| Orchestration / automation                 | 70% |
| Channel adapters                            | 60% (network paths mocked) |
| Desktop                                     | 60% (UI bridge / view-models; rendering owned by Slint) |
| CLI                                         | 50% |

Coverage tracked via `xmake check coverage` (or `llvm-cov` when using Clang).
Reported in `docs/generated/coverage-YYYY-MM-DD.json`.

### What To Test

- Happy path.
- One representative failure per error category.
- Cancellation behavior (every async function has at least one "cancelled mid-flight"
  test).
- Permission gating (one allow, one deny, one ask per gated operation).
- Hook firing order (one test per blocking-vs-advisory).

### Integration Tests

`tests/integration/` exercises end-to-end paths:

- Bootstrap → agent loop → mock provider → mock tool → response.
- Bootstrap → desktop bridge → agent loop → streaming `EventSink` → response.
- Bootstrap → automation tick → agent run.

Integration tests use **mock providers** by default. Real-provider tests run in a
nightly job (gated by env var `ORAN_TEST_REAL_PROVIDERS=1`).

## Benchmarks

### Framework: nanobench

- Simple, low-overhead microbenchmark library.
- Catch2's `BENCHMARK` macro is available too (it wraps nanobench).
- Output: human-readable + machine-readable JSON.

### Bench Structure

Each bench bucket has at least:

- `bench/<lib>/main.cpp` — entry point (registers benches with the runner).
- `bench/<lib>/scenarios/<scenario>.cpp` — one scenario per file.
- `bench/<lib>/README.md` — describes what scenarios exist and what they compare.

### A-vs-B Pattern (Core Convention)

When a design choice has plausible alternatives, the bench compares them:

```cpp
// bench/memory/scenarios/search-fts5-vs-vector.cpp
TEST_CASE("memory.search: FTS5 baseline vs vector", "[bench]") {
  auto corpus = make_test_corpus(/* records= */ 10'000);

  BENCHMARK_ADVANCED("fts5 baseline")(Catch::Benchmark::Chronometer m) {
    auto store = make_fts5_store(corpus);
    m.measure([&] { return store.search("react agent loop", /*limit=*/10); });
  };

  BENCHMARK_ADVANCED("vector cosine")(Catch::Benchmark::Chronometer m) {
    auto store = make_vector_store(corpus);
    m.measure([&] { return store.search("react agent loop", /*limit=*/10); });
  };
}
```

Each scenario emits one line of nanobench output; `compare.cpp` runs all scenarios
and prints a comparison summary.

### When To Benchmark — And When Not To

The bucket-level "≥ 1 A-vs-B" floor (see "A-vs-B Pattern" above and
[`critical-rules.md#C12`](critical-rules.md)) is a *baseline* rule: every
library ships at least one comparison so reviewers have a yardstick. This
subsection covers the *developer-side* decision — when, mid-implementation,
you reach for a bench rather than just writing the code.

**Bench when** you genuinely cannot rank the alternatives by reading:

- Two plausibly correct implementations exist and you do not know which is
  faster (e.g. linear scan over a 16-entry vec vs. `std::unordered_map`;
  prepared-statement cache vs. recompiling per call).
- A refactor changes a data layout or a dispatch shape, and the intuition
  that "this should be faster" needs evidence before merging.
- A perf bug report names a specific hotspot; confirm before optimizing
  blindly.

**Do not bench when**:

- The choice is obvious by reading (O(1) lookup vs. O(n) scan at known
  large n).
- The code is not on a hot path (config load, one-shot CLI startup, doc
  generation).
- The open question is correctness, not speed. Benches measure speed,
  not correctness.
- Every new function. Don't bench-saturate — benches cost compile time
  too (see [`compile-budget.md`](compile-budget.md)) and dilute the
  signal of the benches that matter.

### Reading Bench Results — Speed Is Not The Only Signal

Bench numbers carry jitter. A small delta across two runs on the same
binary is noise; treat the **median across enough iterations** as the
signal, not a single number. Beyond that:

- **Small delta → prefer the simpler / more elegant code.** When the
  candidates are within roughly 10% of each other (and the jitter band
  overlaps), pick the one with less code, fewer concepts, and shallower
  abstractions. The reader who later debugs this pays the complexity
  cost forever; the runtime saves only the delta.
- **Bigger delta with bigger code → name the trade-off.** A 2× speedup
  that triples the line count and adds a cache-invalidation surface may
  still be the right call, but the history's *Design Intent* section
  must say so. The reader needs to know what the speed cost.
- **Speed is one axis.** Memory footprint, allocation count, cache-line
  behavior, branch predictability, and binary size are also load-bearing.
  nanobench reports timing; supplement with `perf stat` or
  `valgrind --tool=callgrind` when the trade-off needs more axes than
  time.
- **Disclose jitter sources.** The bench output JSON already names the
  machine; do not compare numbers across machines without normalizing.

### Optimization Avenues Before "Just Write A Faster Loop"

When a bench shows a real, reproducible gap and the simpler alternative
isn't fast enough, the *order* of optimization to reach for:

1. **Algorithmic / logical optimization.** Reduce big-O. A different data
   structure (sorted vec + binary search vs. linear scan; B-tree vs. hash
   when ordered iteration matters), a different algorithm, or eliminating
   redundant work in a loop. Almost always the biggest single win and
   the cheapest to maintain.
2. **Cache-friendly data layout.** Array-of-structs → struct-of-arrays
   for hot loops; sort by access pattern; pack flags into a bitset;
   keep hot fields together in a cache line; avoid pointer-chasing in
   inner loops.
3. **Precomputation / memoization.** Hoist invariants out of loops;
   cache derived values when source data changes infrequently. The
   tool-catalog renderer in [`prompt-design.md`](prompt-design.md)
   memoizes per-`ToolDef` blocks for exactly this reason.
4. **Parallelism — last resort, not first.** Multi-core via
   `asio::thread_pool`, or spreading work across strands. This is the
   most expensive option (synchronization cost, cache-line bouncing,
   debugging difficulty); it pays off only when the single-threaded
   baseline is already cache-friendly and the work is genuinely
   independent. **Never** as a first-line fix.

Each tier is cheaper to maintain than the one below it. Walk down the
list, stop when the bench is satisfied. Going straight to tier 4
("throw threads at it") is the most common perf mistake in legacy
`orangutan/`; v2 reverses the order.

### Bench Categories

| Category | Library | Compares |
| --- | --- | --- |
| Provider cache mapping | provider | cache hints enabled vs. disabled |
| File read wrapper | io | direct `std::ifstream` vs. coroutine wrapper |
| SQLite insert path | storage | literal execute inserts vs. prepared statement binding |
| SQLite migration path | storage | cold migration apply vs. no-op migration check |
| Config loading | config | in-memory JSON parse vs. checked-in config file load |
| CLI dispatch | cli | single-shot prompt dispatch vs. empty REPL shell dispatch |
| Bootstrap config startup | bootstrap | missing default config fallback vs. explicit config file load |
| Search backend | memory | FTS5 vs. sqlite-vec vs. external HTTP API |
| Dispatch overhead | tool | static lookup vs. hashmap |
| Permission eval | permission | rule-tree vs. linear scan |
| Skill catalog renderer | skill | order-trusting metadata concat vs. deterministic catalog render |
| Mailbox throughput | orchestration | bounded channel vs. asio::channel native |
| Strategy cost | orchestration | leader-worker vs. vote |
| Coroutine handoff | async | direct coroutine post loop vs. bounded `Channel<T>` |
| Compile time | compile-time | pimpl vs. inline private members |

### Bench Output Format

```json
{
  "version": 1,
  "ran_at": "2026-05-14T10:23:42Z",
  "machine": { "cpus": 8, "ram_gb": 16, "model": "..." },
  "results": [
    {
      "bench": "memory.search.fts5_baseline",
      "iters": 1000,
      "median_ns": 124000,
      "p95_ns":    180000,
      "p99_ns":    250000
    },
    ...
  ]
}
```

Stored under `docs/generated/bench-YYYY-MM-DD.json`. CI's nightly job runs the
benchmark suite and updates the file via PR.

### Comparison Runner

`scripts/bench-compare.sh <library>`:

- Builds `bench-<library>` if needed.
- Runs it.
- Loads previous baseline from `docs/generated/bench-baseline-<library>.json`.
- Prints a table: scenario | median | delta vs. baseline.
- Exits non-zero if any scenario regressed > 10%.

## CI Wiring

- `make ci` (docs + hygiene + shell-lint) on every PR.
- `xmake test` on every PR.
- `xmake build bench-*` smoke-built on every PR (compile-only; does not run benches).
- **Nightly**: full bench run, compile-budget recheck, real-provider integration
  (gated).
- **Pre-merge**: maintainer manually triggers a bench compare for `perf-impact` PRs.

## Expectations Per Change

| Change type | Tests | Benches |
| --- | --- | --- |
| Bug fix | regression test | (none unless perf bug) |
| New feature | tests for happy path + failures + cancellation | one bench if perf is plausibly affected |
| Refactor | existing tests pass | bench delta documented if any |
| Perf improvement | tests pass | mandatory bench A/B; commit message includes numbers |
| Build / packaging | build & test pass | (none) |
| Doc-only | (none) | (none) |

## Anti-Patterns

- "We'll add benches later." Add one at submission time, even if minimal.
- Benches that don't compare anything. The "A-vs-B" pattern keeps benches meaningful.
- Tests that depend on real network calls without an opt-in env var.
- Tests that use `std::this_thread::sleep_for` to "wait for async to finish". Drive
  an `asio::io_context` with a hard timeout, as `tests/async/test_async.cpp` does.

## See Also

- [`compile-budget.md`](compile-budget.md) — compile-time is also benched.
- [`../product-specs/0010-benchmark-harness.md`](../product-specs/0010-benchmark-harness.md)
  — concrete v1 deliverable.

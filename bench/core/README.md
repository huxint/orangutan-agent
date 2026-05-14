# `bench/core/` — nanobench scenarios for `oran-core`

## What this bucket benchmarks

`oran-core` is the project's foundation: `Error`, `Result<T>`, `all_ok`. The bench
bucket exists for parity ([`docs/rules/critical-rules.md#C12`](../../docs/rules/critical-rules.md))
and to anchor the A-vs-B convention even on a library where performance is not the
primary axis.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/error_construct.cpp`](scenarios/error_construct.cpp) | Fluent builder + `.with` chain *vs.* move-only constructor + sequential `.with` calls. Both produce the identical `Error` value; we measure whether the builder-return-value path costs more or less than the explicit local-object path. |

## Running

```sh
xmake build bench-core
xmake run bench-core
```

Output is nanobench's `markdown` shape on stdout. The first run on a given machine is
the baseline; subsequent runs report deltas. Slice 0 does not yet wire CI to track
this baseline — that wiring lands with `bench/async/` (the first scenario whose
delta would meaningfully affect operations).

## See Also

- [`docs/rules/testing-and-bench.md`](../../docs/rules/testing-and-bench.md)
- [`docs/product-specs/0010-benchmark-harness.md`](../../docs/product-specs/0010-benchmark-harness.md)

# `bench/core/` — nanobench scenarios for `oran-core`

## What this bucket benchmarks

`oran-core` is the project's foundation: `Error`, `Result<T>`, `all_ok`, `Time`,
and the ISO-8601 UTC format/parse helpers. The bench bucket exists for parity
([`docs/rules/critical-rules.md#C12`](../../docs/rules/critical-rules.md))
and to anchor the A-vs-B convention even on a library where performance is not the
primary axis.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/error_construct.cpp`](scenarios/error_construct.cpp) | Fluent builder + `.with` chain *vs.* move-only constructor + sequential `.with` calls. Both produce the identical `Error` value; we measure whether the builder-return-value path costs more or less than the explicit local-object path. |
| [`scenarios/time.cpp`](scenarios/time.cpp) | `core::time::format_iso8601_utc` (explicit `{:04}-...` template) *vs.* `std::format("{:%FT%T}Z", floor<ms>(tp))` (chrono format specifiers). Both render the canonical wire format; the comparison documents the cost of the project's deterministic-wire-format guarantee. The same file also benches `core::time::parse_iso8601_utc` so downstream callers can see the parse-vs-format budget when they round-trip timestamps. |

## Running

```sh
xmake build bench-core
xmake run bench-core
```

Output is nanobench's `markdown` shape on stdout. The first run on a given machine is
the baseline; subsequent runs report deltas. CI baseline tracking lands with the
benchmark-harness baseline slice.

## See Also

- [`docs/rules/testing-and-bench.md`](../../docs/rules/testing-and-bench.md)
- [`docs/product-specs/0010-benchmark-harness.md`](../../docs/product-specs/0010-benchmark-harness.md)

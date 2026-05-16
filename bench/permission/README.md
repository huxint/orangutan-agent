# `bench/permission/` — nanobench scenarios for `oran-permission`

## What this bucket benchmarks

`oran-permission` ships the foundation rule-evaluator surface
(`RuleSet::evaluate` over a flat list of `Rule`s with the
deny → allow → ask precedence) and a capability-aware overload that
filters matching by `core::Capability`. The bench bucket exists for
parity
([`docs/rules/critical-rules.md#C12`](../../docs/rules/critical-rules.md))
and anchors the A-vs-B convention as the future regex / HMAC / audit
slices land.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/rule_set.cpp`](scenarios/rule_set.cpp) | `RuleSet::evaluate` precedence walk (deny → allow → ask, three passes) over a 16-rule fixture *vs.* `std::ranges::find_if` single-pass first-match scan on the same rules. The precedence-respecting walk does up to three passes plus a formatted-reason build; the find_if path is the cheapest possible matcher and ignores precedence. Documents the cost of the foundation evaluator. The same file also registers `permission.rule_set_capability_match` *vs.* `permission.rule_set_capability_miss` over a separate 16-rule capability-scoped fixture: the match path fires a capability-bound rule the call satisfies (formats a `capability=<name>` reason); the miss path runs the same precedence walk but every capability-bound rule's scope excludes the call, so the walk falls through to the mode default. Documents the cost of the optional-capability check on both the success and miss sides. |

## Running

```sh
xmake build bench-permission
xmake run bench-permission
```

## See Also

- [`docs/rules/testing-and-bench.md`](../../docs/rules/testing-and-bench.md)
- [`docs/product-specs/0010-benchmark-harness.md`](../../docs/product-specs/0010-benchmark-harness.md)

# `bench/permission/` — nanobench scenarios for `oran-permission`

## What this bucket benchmarks

`oran-permission` ships the foundation rule-evaluator surface
(`RuleSet::evaluate` over a flat list of `Rule`s with the
deny → allow → ask precedence), a capability-aware overload that
filters matching by `core::Capability`, the `Defaults::for_mode`
factory that returns the safe baseline `RuleSet` per mode (layer-1
of the design-doc three-layer rule merge), the three-layer
`materialize(Mode, global, per_agent) -> RuleSet` that
concatenates defaults + global config + per-agent overlay, the
`InputPattern` runtime-regex wrapper (re2) that gates rule
matching on the call's `input` string, and the `ApprovalSecret`
HMAC-SHA-256 wrapper (libsodium) that backs the ask-flow
approval signing path. The bench bucket exists for parity
([`docs/rules/critical-rules.md#C12`](../../docs/rules/critical-rules.md))
and anchors the A-vs-B convention as the future HMAC / audit
slices land.

## Scenarios

| File | A vs. B |
| --- | --- |
| [`scenarios/rule_set.cpp`](scenarios/rule_set.cpp) | `RuleSet::evaluate` precedence walk (deny → allow → ask, three passes) over a 16-rule fixture *vs.* `std::ranges::find_if` single-pass first-match scan on the same rules. The precedence-respecting walk does up to three passes plus a formatted-reason build; the find_if path is the cheapest possible matcher and ignores precedence. Documents the cost of the foundation evaluator. The same file also registers `permission.rule_set_capability_match` *vs.* `permission.rule_set_capability_miss` over a separate 16-rule capability-scoped fixture: the match path fires a capability-bound rule the call satisfies (formats a `capability=<name>` reason); the miss path runs the same precedence walk but every capability-bound rule's scope excludes the call, so the walk falls through to the mode default. Documents the cost of the optional-capability check on both the success and miss sides. |
| [`scenarios/defaults.cpp`](scenarios/defaults.cpp) | `Defaults::for_mode(Mode::default_)` factory call *vs.* an inline build of the same 9-rule baseline. Documents the cost of the factory (one function-call frame + one `RuleSet` move out) against bare inline construction so future config-loading paths know the factory is essentially free at startup. |
| [`scenarios/materialize.cpp`](scenarios/materialize.cpp) | Three scenarios sharing the same `Mode::default_` defaults: `permission.materialize_defaults_only` (empty global + empty agent), `permission.materialize_with_global` (8-rule global), and `permission.materialize_with_global_and_agent` (8-rule global + 2-rule per-agent overlay). The first anchors the cost of the layer-concatenation surface, the second measures per-rule append cost from `config::PermissionRuleConfig`, and the third documents the full three-layer merge cost a future `oran-bootstrap` per-agent assembly path will pay. |
| [`scenarios/input_pattern.cpp`](scenarios/input_pattern.cpp) | Three scenarios over a single-rule fixture (`shell.exec` + `deny`): `permission.input_pattern_match` evaluates the rule with an input the re2 pattern accepts (pays re2's `PartialMatch` on the success path plus the formatted reason build); `permission.input_pattern_miss` evaluates the same rule with non-matching input (pays `PartialMatch` on the failure path and the shorter "default by mode=" fallback reason); `permission.no_input_pattern` evaluates a rule shape that drops the `input_pattern` altogether (anchors the cost *removed* by skipping the re2 hop). Together they document the input-regex budget end-to-end. |
| [`scenarios/approval_secret.cpp`](scenarios/approval_secret.cpp) | Four scenarios over a fixed 32-byte key: `permission.hmac_short_message` MACs a 32-byte payload (realistic approval-token size); `permission.hmac_long_message` MACs a 1 KiB payload (per-byte hash cost growth axis); `permission.hmac_macs_equal_ok` runs `ApprovalSecret::macs_equal` on two identical 32-byte MACs; `permission.hmac_macs_equal_no` runs it on a pair that differs in the last byte, anchoring the constant-time-compare guarantee that match and miss cost the same. Together they document the libsodium HMAC-SHA-256 budget end-to-end, which the future approval-token slice will build on. |

## Running

```sh
xmake build bench-permission
xmake run bench-permission
```

## See Also

- [`docs/rules/testing-and-bench.md`](../../docs/rules/testing-and-bench.md)
- [`docs/product-specs/0010-benchmark-harness.md`](../../docs/product-specs/0010-benchmark-harness.md)

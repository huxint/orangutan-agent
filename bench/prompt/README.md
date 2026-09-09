# bench-prompt

`bench-prompt` measures pure prompt rendering and native tool fingerprinting.
`prompt.render_native_catalog` uses all seven built-in definitions;
`prompt.render_native_subset` uses two definitions from the same fixture.
Neither path constructs an executor, discovers tools or renders schema text.

Prompt tests cover stable prefixes across conversation changes and cache
invalidation when native definitions change. Tool selection has its own
benchmarks in [bench-tool](../tool/README.md).

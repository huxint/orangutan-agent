# `bench/skill`

Skill benchmarks cover the prompt section-4 catalog renderer.

| Scenario | Compares |
| --- | --- |
| [`scenarios/catalog.cpp`](scenarios/catalog.cpp) | A local loader-order baseline vs. the production renderer's validation plus deterministic name sort for a 32-skill metadata snapshot. |

The renderer keeps skill bodies out of the catalog, so this bucket measures only
compact metadata rendering. The loader snapshot and one-shot `skill.invoke`
lookup are correctness-oriented startup/dispatch work; watcher benchmarks land
with that slice if a real tradeoff appears.

# `bench/agent`

Agent benchmarks cover session-owned state that will sit on the ReAct loop's
hot path.

| Scenario | Compares |
| --- | --- |
| [`scenarios/prompt_cache_hit_rate.cpp`](scenarios/prompt_cache_hit_rate.cpp) | Stable prompt builds without promotions vs. builds after `tool.search` promotes one deferred tool. |

The prompt-cache scenario asserts that `RenderedPrompt::prefix_hash` stays
identical across iterations after the promotion snapshot is fixed; timing is
secondary to catching accidental cached-prefix drift.

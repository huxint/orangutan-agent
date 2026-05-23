# `bench-provider`

Provider benchmarks pin the internal adapter-facing overhead before real
network transports land.

| Scenario | What it compares |
| --- | --- |
| [`scenarios/cache_mapping.cpp`](scenarios/cache_mapping.cpp) | `provider.cache_hints_enabled` validates and maps a `prompt::RenderedPrompt` prefix into adapter cache keys, while `provider.cache_hints_disabled` is the route-level off switch baseline. |

# Feature Release Notes

## 2026-05

| Date | Area | User Impact | Change Summary | History |
| ---- | ---- | ----------- | -------------- | ------- |
| 2026-05-14 | build-skeleton | First working `orangutan` binary lands; project compiles end-to-end as C++26 on GCC 16.1 with `xmake`. `xmake run orangutan` prints a greeting via `std::print`. | Slice 0: language ratchet to C++26 (rule C17), GCC `-fanalyzer` wiring + rule (C18 / `static-analysis.md`), `oran-core` lib (`Error` / `Result<T>` / `all_ok`), `tests/core` (8 cases, 47 assertions, all pass), `bench/core` (A-vs-B scenario). Clean build 4.37 s. | [history](../histories/2026-05/20260514-2214-mvp-build-skeleton-slice-0.md) |
| 2026-05-14 | scaffold | Introduced the harness-engineering scaffold for the Orangutan v2 rewrite. | Added the agent entry docs, design docs, rules, product specs, exec-plan + history templates, scripts, and CI scaffolding. | (initial scaffold) |

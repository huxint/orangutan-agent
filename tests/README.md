# `tests/` — Catch2 Test Buckets

One subdirectory per library: `tests/<lib>/`. Each bucket is an independent xmake
target so iterating on `tests/<lib>/` does not recompile other buckets.

## Layout

```
tests/
├── test-helpers/         # shared helpers: unique paths, mocks, fakes
│   ├── unique-paths.hpp
│   ├── run_async.hpp
│   ├── scoped-env.hpp
│   ├── fake-provider.hpp
│   └── ...
├── core/
├── async/
├── log/
├── io/
├── http/
├── storage/
├── config/
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
├── channel-qq/
├── web/
├── cli/
├── bootstrap/
└── integration/          # end-to-end paths with mocked externals
```

## Conventions

- Catch2 v3. `[unit]` / `[integration]` / `[property]` tags.
- One file per scenario / feature; small. Don't create monoliths.
- Use `test-helpers/` for shared setup; do not duplicate fixtures.
- The live buckets include `tests/skill/` for the section-4 skill catalog
  renderer/owner, the markdown loader snapshot, and workspace snapshot
  hot-reload; `tests/tool/` and `tests/bootstrap/` cover the `skill.invoke`
  dispatch path plus runner prompt-boundary refresh.
- See [`../docs/rules/testing-and-bench.md`](../docs/rules/testing-and-bench.md) for
  the full convention.

## Running

```sh
xmake test                    # all buckets
xmake run test-agent          # one bucket
xmake run test-agent "[unit]" # tag filter
xmake run test-agent "specific test name"
```

## Status

`tests/core/`, `tests/async/`, `tests/http/`, `tests/io/`, `tests/storage/`, `tests/config/`,
`tests/permission/`, `tests/hook/`, `tests/memory/`, `tests/skill/`, `tests/tool/`,
`tests/prompt/`, `tests/provider/`, `tests/agent/`, `tests/cli/`, and
`tests/bootstrap/` are live and registered with `xmake test`.
Additional buckets land with their owning libraries.

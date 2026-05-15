## [2026-05-15 22:45] | Task: `oran-config` foundation

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI, repo on Linux/WSL2, GCC 16.1.1 system compiler.
- Linked plan: `docs/exec-plans/completed/2026-05-15-oran-config-foundation.md`

### User Query

> 开始执行下一阶段

### Changes Overview

- Areas: `oran-config` public API, JSON parser implementation, xmake targets,
  example config, tests/config, bench/config, config/security docs.
- Key actions:
  - Added `oran-config` with `Config::parse(std::string_view, LoadOptions)` and
    `Config::load_file(std::string_view, LoadOptions)`.
  - Added typed config fields for `strict_config`, `runtime`, `profiles`, `routes`,
    `session`, and `web`.
  - Implemented recursive string substitution for `${VAR}` and `${VAR:-default}`.
  - Added warning-by-default and strict-error handling for unknown root fields.
  - Added load-tested `config.example.json`.
  - Added `test-config` (5 cases / 49 assertions) and `bench-config` parse-vs-file-load
    coverage.
  - Bumped the early `orangutan` binary checkpoint to `2.0.0-slice5`.
  - Follow-up review fixes: loose unknown root subtrees no longer trigger env
    substitution failures, config env tests are hermetic against ambient variables,
    and `docs/design-docs/api-portability.md` quotes the current flat profile shape.

### Design Intent

This slice deliberately keeps the public API small and expected-only. `nlohmann_json`
is confined to `src/oran-config/config.cpp`, while public headers expose plain C++
types so downstream libraries do not inherit JSON parser compile cost. Secret crypto
and generated JSON Schema are documented as follow-up work instead of being implied by
the first loader.

### Files Modified

- `include/oran/config.hpp`, `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `config.example.json`
- `tests/config/*`
- `bench/config/*`
- `xmake/{packages,targets,tests,bench}.lua`, `xmake-requires.lock`
- `src/main.cpp`
- Docs listed below.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — slice status and `oran-config` boundary updated.
- `docs/design-docs/secrets-and-state.md` — current config file shape, typed fields,
  env substitution, unknown-root behavior, and schema future state documented.
- `docs/design-docs/api-portability.md` — provider profile example synced to the
  current `config.example.json` / parser shape.
- `docs/BUILD_SYSTEM.md`, `docs/rules/libraries.md` — `nlohmann_json 3.12.0` package
  and target wiring documented.
- `docs/SECURITY.md`, `docs/rules/critical-rules.md` — current no-secret-crypto status
  and future secret handling clarified.
- `include/README.md`, `src/README.md`, `tests/README.md`, `bench/README.md` — live
  config library/test/bench buckets recorded.
- `docs/rules/testing-and-bench.md`,
  `docs/product-specs/0010-benchmark-harness.md` — config A-vs-B bench documented.
- `docs/QUALITY_SCORE.md`, `docs/exec-plans/tech-debt-tracker.md`,
  `docs/releases/feature-release-notes.md` — status, remaining debt, and release note
  updated.

### Validation

- Commands run:
  ```sh
  xmake f -m release -y
  xmake build oran-config
  xmake build test-config
  xmake build bench-config
  xmake build orangutan
  xmake run orangutan
  xmake run test-config
  xmake run bench-config
  xmake test
  make ci
  scripts/check-lib-parity.sh
  git diff --check
  xmake f -m release --analyze=y
  xmake build -r oran-config
  xmake f -m release --analyze=n
  ```
- Tests added/changed:
  - `tests/config/test_config.cpp`: typed value parsing, env substitution with
    defaults, config-error returns, loose unknown-root env skipping, unknown-root
    strict handling, hermetic example fallback coverage, and checked-in
    `config.example.json` loading.
- Bench impact:
  - `bench/config`: in-memory parse vs. file load.
  - Latest local run:
    - `config.parse_memory`: 6,397.44 ns/load
    - `config.load_file_example`: 12,527.54 ns/load

### Follow-ups

- Issues opened: none.
- Tech-debt updated: generated `docs/generated/config.schema.json` is still planned.
- Linked release note: `docs/releases/feature-release-notes.md` (2026-05 row).

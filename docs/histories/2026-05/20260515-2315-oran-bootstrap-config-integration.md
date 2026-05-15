## [2026-05-15 23:15] | Task: `oran-bootstrap` config integration

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI, repo on Linux/WSL2, GCC 16.1.1 system compiler.
- Linked plan: `docs/exec-plans/completed/2026-05-15-oran-bootstrap-config-integration.md`

### User Query

> oran-bootstrap config integration

### Changes Overview

- Areas: `oran-bootstrap` public API, `orangutan` binary handoff, config CLI parsing,
  tests/bootstrap, bench/bootstrap, docs.
- Key actions:
  - Added `bootstrap::load_config(BootstrapOptions)` and `bootstrap::run(BootstrapOptions)`.
  - Added `--config <path>`, `--config=<path>`, `--help`, and `-h` parsing.
  - Resolved default config as `<workspace>/.orangutan/config.json`.
  - Loaded the default file when present, used built-in config defaults when absent,
    and treated explicit `--config` as required.
  - Wired `src/main.cpp` through `oran-bootstrap` and bumped the banner to
    `2.0.0-slice6`.
  - Added `test-bootstrap` and `bench-bootstrap` coverage.

### Design Intent

This is a deliberately narrow bootstrap slice. It establishes the process entry
boundary and config load behavior without pretending the REPL, provider runtime,
storage assembly, signal handling, or agent loop exist. Explicit config paths are
strict so operator typos do not silently fall back to defaults; the implicit default
path remains optional so a fresh checkout still runs.

### Files Modified

- `include/oran/bootstrap.hpp`, `include/oran/bootstrap/bootstrap.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/main.cpp`
- `tests/bootstrap/*`
- `bench/bootstrap/*`
- `xmake/{targets,tests,bench}.lua`
- Docs listed below.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/bootstrap-runtime.md` — current bootstrap API, config resolution,
  and binary behavior documented.
- `docs/design-docs/index.md` — design doc catalogue updated.
- `docs/ARCHITECTURE.md` — slice status, bootstrap row, and binary/config behavior
  updated.
- `docs/design-docs/secrets-and-state.md` — default config path and early fallback
  behavior clarified.
- `docs/product-specs/0001-core-react-loop.md` — current pre-loop `--config` support
  and validation commands documented.
- `docs/BUILD_SYSTEM.md`, `include/README.md`, `src/README.md`, `tests/README.md`,
  `bench/README.md`, `docs/rules/testing-and-bench.md`,
  `docs/product-specs/0010-benchmark-harness.md`, `docs/QUALITY_SCORE.md`, and
  `docs/releases/feature-release-notes.md` updated.

### Validation

- Commands run:
  ```sh
  xmake f -m release -y
  xmake build oran-bootstrap
  xmake build test-bootstrap
  xmake build bench-bootstrap
  xmake build orangutan
  xmake run test-bootstrap
  xmake run bench-bootstrap
  xmake run orangutan
  xmake run orangutan -- --config config.example.json
  xmake run orangutan -- --config missing-config.json
  xmake test
  make ci
  scripts/check-lib-parity.sh
  git diff --check
  xmake f -m release --analyze=y
  xmake build -r oran-bootstrap
  xmake f -m release --analyze=n
  ```
  The missing-config smoke is expected to fail the program with exit code `1`; xmake
  reports that as command exit `255`.
- Tests added/changed:
  - `tests/bootstrap/test_bootstrap.cpp`: default-missing built-in fallback, default
    file load, explicit config forms, `xmake run` separator handling, invalid
    arguments, missing explicit config, and help handling.
- Bench impact:
  - `bench/bootstrap`: default-missing built-in config startup vs. explicit config
    file load.
  - Latest local run:
    - `bootstrap.config_missing_default`: 1,299.61 ns/load
    - `bootstrap.config_explicit_file`: 6,954.85 ns/load

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` (2026-05 row).

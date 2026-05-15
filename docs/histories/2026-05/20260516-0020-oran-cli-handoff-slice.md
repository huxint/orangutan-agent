## [2026-05-16 00:20] | Task: `oran-cli` handoff slice

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI, repo on Linux/WSL2, GCC 16.1.1 system compiler.
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-cli-handoff-slice.md`

### User Query

> 实现 oran-cli handoff slice

### Changes Overview

- Areas: `oran-cli` public API, bootstrap CLI handoff, `orangutan` binary behavior,
  tests/cli, bench/cli, docs.
- Key actions:
  - Added stdlib-only `cli::run(CliOptions)` with `CliMode`, `CliResult`, quiet mode,
    and deterministic pre-agent-loop output.
  - Added `--prompt <text>`, `--prompt=<text>`, CLI help, and default REPL shell mode.
  - Changed bootstrap parsing so `--config` remains bootstrap-owned while CLI args are
    forwarded to `oran-cli` after config loading.
  - Bumped the binary banner to `2.0.0-slice7`.
  - Added `test-cli` and `bench-cli` coverage, plus bootstrap regression tests for the
    handoff.

### Design Intent

This slice creates the CLI boundary without pretending the agent loop or provider
runtime exists. `oran-cli` stays `oran-core`-only and avoids a terminal line-editor
dependency until history/editing behavior is needed. Bootstrap still owns process setup
and config resolution, then delegates user-facing mode selection to CLI.

### Files Modified

- `include/oran/cli.hpp`, `include/oran/cli/cli.hpp`
- `src/oran-cli/cli.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/cli/*`, `tests/bootstrap/test_bootstrap.cpp`
- `bench/cli/*`
- `xmake/{targets,tests,bench}.lua`
- Docs listed below.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/cli-runtime.md` — current CLI API, mode parsing, and bootstrap
  handoff documented.
- `docs/design-docs/bootstrap-runtime.md` — CLI handoff after config load documented.
- `docs/design-docs/index.md` — design doc catalogue updated.
- `docs/ARCHITECTURE.md` — slice status, CLI row, bootstrap dependency row, and config
  handoff behavior updated.
- `docs/product-specs/0001-core-react-loop.md` — current pre-loop `--prompt` and REPL
  shell behavior documented.
- `docs/BUILD_SYSTEM.md`, `include/README.md`, `src/README.md`, `tests/README.md`,
  `bench/README.md`, `docs/rules/testing-and-bench.md`,
  `docs/product-specs/0010-benchmark-harness.md`, `docs/QUALITY_SCORE.md`, and
  `docs/releases/feature-release-notes.md` updated.

### Validation

- Commands run:
  ```sh
  xmake f -m release -y
  xmake build oran-cli
  xmake build oran-bootstrap
  xmake build test-cli
  xmake build bench-cli
  xmake build orangutan
  xmake run test-cli
  xmake run test-bootstrap
  xmake run bench-cli
  xmake run orangutan
  xmake run orangutan -- --prompt "What is 17 * 23?"
  xmake run orangutan -- --config config.example.json --prompt "hello"
  xmake run orangutan -- --unknown
  xmake test
  make ci
  scripts/check-lib-parity.sh
  git diff --check
  xmake f -m release --analyze=y
  xmake build -r oran-cli
  xmake build -r oran-bootstrap
  xmake f -m release --analyze=n
  ```
  The unknown-argument smoke is expected to fail the program with exit code `1`; xmake
  reports that as command exit `255`.
- Tests added/changed:
  - `tests/cli/test_cli.cpp`: default REPL mode, scripted REPL prompt counting,
    explicit prompt forms, `xmake run` separator handling, help precedence, missing /
    empty / duplicate prompt errors, and unknown CLI args.
  - `tests/bootstrap/test_bootstrap.cpp`: config loading now ignores CLI args, and
    `bootstrap::run` forwards prompt args / returns CLI errors after config load.
- Bench impact:
  - `bench/cli`: single-shot prompt dispatch vs. empty REPL shell dispatch.
  - Latest local run:
    - `cli.single_shot_prompt`: 17.17 ns/dispatch
    - `cli.repl_empty`: 7.85 ns/dispatch

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` (2026-05 row).

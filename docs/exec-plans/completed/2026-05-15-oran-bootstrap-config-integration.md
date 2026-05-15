# `oran-bootstrap` — Config Integration

## Goal

Land the first `oran-bootstrap` slice: route the `orangutan` binary through a
bootstrap library that parses the initial config CLI surface, resolves the default
workspace config path, loads `oran-config`, and reports the loaded config source. This
turns the previous main-level placeholder into a reusable bootstrap boundary without
claiming the agent loop is implemented.

## Scope

- In scope:
  - Add public headers under `include/oran/bootstrap/` plus umbrella
    `include/oran/bootstrap.hpp`.
  - Add `oran-bootstrap`, `test-bootstrap`, and `bench-bootstrap` xmake targets.
  - Implement `bootstrap::load_config(BootstrapOptions)` and
    `bootstrap::run(BootstrapOptions)`.
  - Support `--config <path>` and `--config=<path>`.
  - Support `--help` / `-h` as a no-config-load early exit.
  - Resolve the default config path as `<workspace>/.orangutan/config.json`.
  - Load the default config when present; use built-in `config::Config{}` defaults
    when the default file is absent in this early runtime slice.
  - Treat explicit `--config` as required: missing/unreadable/invalid explicit config
    returns `core::Result` errors.
  - Wire `src/main.cpp` to call `bootstrap::run`.
  - Add tests, a config-startup A-vs-B bench, docs, release note, and history.
- Out of scope:
  - REPL / `--prompt` implementation.
  - Provider runtime assembly.
  - Storage pool or database file creation.
  - Secret decryption, rotation, or provider credential reads.
  - Signal handling and cancellation.

## Context

- Relevant docs:
  - `docs/ARCHITECTURE.md`
  - `docs/design-docs/secrets-and-state.md`
  - `docs/product-specs/0001-core-react-loop.md`
  - `docs/rules/error-handling.md`
  - `docs/rules/testing-and-bench.md`
  - `docs/rules/docs-in-sync.md`
- Relevant code paths:
  - New `include/oran/bootstrap/*`, `src/oran-bootstrap/*`
  - `src/main.cpp`
  - `xmake/{targets,tests,bench}.lua`
  - New `tests/bootstrap/*`, `bench/bootstrap/*`
- Constraints:
  - Public APIs return `core::Result<T>`.
  - This slice must keep `xmake run orangutan` usable in a repo with no
    `.orangutan/config.json`.
  - No secret values are read or logged.

## Milestones

1. Add active plan and directory scaffolding.
2. Add public API and xmake wiring.
3. Implement config CLI parsing, path resolution, loading, and binary handoff.
4. Add tests and bench.
5. Update production docs, quality score, release notes, and history.
6. Run validation, complete audit, and move this plan to completed.

## Validation

- Commands:
  - `xmake f -m release -y`
  - `xmake build oran-bootstrap`
  - `xmake build orangutan`
  - `xmake run test-bootstrap`
  - `xmake test`
  - `xmake run bench-bootstrap`
  - `make ci`
  - `scripts/check-lib-parity.sh`
  - `git diff --check`
  - `xmake f -m release --analyze=y`
  - `xmake build -r oran-bootstrap`
  - `xmake f -m release --analyze=n`
- Manual checks:
  - `xmake run orangutan` succeeds with no default config file.
  - `xmake run orangutan -- --config config.example.json` reports explicit config
    loading.
  - Explicit missing config returns a non-zero binary exit.
- Bench comparison:
  - `bench/bootstrap` compares missing-default built-in config startup vs. explicit
    checked-in config file load.

## Progress Log

- [x] Confirm next phase as `oran-bootstrap` config integration.
- [x] Add build/package wiring and public API.
- [x] Implement config CLI parsing and load behavior.
- [x] Add tests and bench.
- [x] Update docs that this slice invalidates in the same PR.
- [x] Run validation and record results.
- [x] Update `docs/QUALITY_SCORE.md`.
- [x] Write history entry.
- [x] Add release note.
- [x] Move this plan to `docs/exec-plans/completed/` before finish.

## Decision Log

- 2026-05-15: Keep default config absence non-fatal. Rationale: this early binary must
  remain runnable in a fresh checkout while still loading `<workspace>/.orangutan/config.json`
  automatically when operators create it.
- 2026-05-15: Make explicit `--config` strict. Rationale: when an operator names a
  config file, silently falling back to defaults hides misconfiguration.

## Linked Artifacts

- Related design doc: `docs/design-docs/bootstrap-runtime.md`
- Related config doc: `docs/design-docs/secrets-and-state.md`
- Related product spec: `docs/product-specs/0001-core-react-loop.md`
- History entry: `docs/histories/2026-05/20260515-2315-oran-bootstrap-config-integration.md`
- Release note: `docs/releases/feature-release-notes.md`

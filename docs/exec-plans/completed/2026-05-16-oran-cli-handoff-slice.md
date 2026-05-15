# `oran-cli` — Handoff Slice

## Goal

Land the first `oran-cli` slice: route CLI-facing process modes out of
`oran-bootstrap` after config loading, support a deterministic `--prompt` single-shot
shell and a minimal deterministic REPL shell, and keep the implementation explicitly pre-agent
loop.

## Scope

- In scope:
  - Add public headers under `include/oran/cli/` plus umbrella `include/oran/cli.hpp`.
  - Add `oran-cli`, `test-cli`, and `bench-cli` xmake targets.
  - Implement a small `cli::run(CliOptions)` API returning `core::Result<CliResult>`.
  - Support `--prompt <text>` and `--prompt=<text>` as single-shot mode.
  - Treat no CLI mode arguments as REPL mode.
  - Support `--help` / `-h` at the CLI layer without loading provider runtime.
  - Let `oran-bootstrap` consume only bootstrap-owned flags (`--config`, `--help`,
    `--`) and pass remaining arguments to `oran-cli` after config loading.
  - Print config source/summary once from bootstrap before handing off.
  - Add focused tests, a CLI dispatch A-vs-B bench, docs, release note, and history.
- Out of scope:
  - Provider adapters.
  - Agent loop execution.
  - Streaming token renderer backed by a model.
  - Slash-command registry.
  - Line editor dependency such as `replxx`; this slice stays stdlib-only.
  - Storage/session creation.

## Context

- Relevant docs:
  - `docs/ARCHITECTURE.md`
  - `docs/design-docs/bootstrap-runtime.md`
  - `docs/product-specs/0001-core-react-loop.md`
  - `docs/rules/error-handling.md`
  - `docs/rules/testing-and-bench.md`
  - `docs/rules/docs-in-sync.md`
- Relevant code paths:
  - New `include/oran/cli/*`, `src/oran-cli/*`
  - `src/oran-bootstrap/bootstrap.cpp`
  - `xmake/{targets,tests,bench}.lua`
  - New `tests/cli/*`, `bench/cli/*`
- Constraints:
  - Public APIs return `core::Result<T>`.
  - Public headers avoid heavy dependencies.
  - The binary must remain runnable with no `.orangutan/config.json`.
  - `--prompt` must no longer be rejected by bootstrap.
  - No provider credentials are read or logged.

## Risks

- Risk: CLI parsing duplicated between bootstrap and CLI.
  - Mitigation: bootstrap only strips bootstrap-owned flags and passes a span of
    remaining args to `oran-cli`.
- Risk: REPL shell implies a real agent loop.
  - Mitigation: output text explicitly says the agent loop is not implemented yet.
- Risk: no line editor makes the REPL feel bare.
  - Mitigation: document `replxx` as a future dependency; do not add it until the REPL
    needs history/editing behavior.

## Milestones

1. Add active plan and CLI scaffolding.
2. Add public API and xmake wiring.
3. Implement CLI mode parsing and deterministic shell behavior.
4. Update bootstrap handoff to load config before invoking CLI.
5. Add tests and bench.
6. Update production docs, quality score, release note, and history.
7. Run validation, complete audit, and move this plan to completed.

## Validation

- Commands:
  - `xmake f -m release -y`
  - `xmake build oran-cli`
  - `xmake build orangutan`
  - `xmake run test-cli`
  - `xmake run test-bootstrap`
  - `xmake test`
  - `xmake run bench-cli`
  - `make ci`
  - `scripts/check-lib-parity.sh`
  - `git diff --check`
  - `xmake f -m release --analyze=y`
  - `xmake build -r oran-cli`
  - `xmake build -r oran-bootstrap`
  - `xmake f -m release --analyze=n`
- Manual checks:
  - `xmake run orangutan` selects the deterministic REPL shell and exits.
  - `xmake run orangutan -- --prompt "What is 17 * 23?"` reaches single-shot mode.
  - `xmake run orangutan -- --config config.example.json --prompt "hello"` loads the
    explicit config and reaches single-shot mode.
  - Unknown CLI args return a non-zero binary exit.
- Bench comparison:
  - `bench/cli` compares prompt single-shot dispatch vs. empty REPL dispatch shell.

## Progress Log

- [x] Confirm next phase as `oran-cli` handoff slice.
- [x] Add build/package wiring and public API.
- [x] Implement CLI parsing and shell behavior.
- [x] Update bootstrap handoff.
- [x] Add tests and bench.
- [x] Update docs that this slice invalidates in the same PR.
- [x] Run validation and record results.
- [x] Update `docs/QUALITY_SCORE.md`.
- [x] Write history entry.
- [x] Add release note.
- [x] Move this plan to `docs/exec-plans/completed/` before finish.

## Decision Log

- 2026-05-16: Keep this slice stdlib-only. Rationale: `replxx` is approved for
  `oran-cli`, but the current handoff only needs deterministic input/output behavior
  that can be tested without a terminal line editor.

## Linked Artifacts

- Related bootstrap design doc: `docs/design-docs/bootstrap-runtime.md`
- Related product spec: `docs/product-specs/0001-core-react-loop.md`
- History entry: `docs/histories/2026-05/20260516-0020-oran-cli-handoff-slice.md`
- Release note: `docs/releases/feature-release-notes.md`

# `oran-config` — Config Foundation

## Goal

Land the first `oran-config` slice: a small expected-only JSON config loader with typed
runtime/provider/route/session/web fields, recursive environment substitution, strict
unknown-field behavior, a checked `config.example.json`, tests, and a parse-vs-file-load
benchmark. This creates the config foundation that `oran-bootstrap` can consume later.

## Scope

- In scope:
  - Add `nlohmann_json 3.12.0` to xmake requirements for `oran-config`.
  - Add public headers under `include/oran/config/` plus umbrella
    `include/oran/config.hpp`.
  - Add `oran-config`, `test-config`, and `bench-config` xmake targets.
  - Implement `Config::parse(std::string_view, LoadOptions)` and
    `Config::load_file(std::string_view, LoadOptions)`.
  - Implement recursive string environment substitution for `${VAR}` and
    `${VAR:-default}`.
  - Parse a conservative typed surface: `strict_config`, `runtime`, `profiles`,
    `routes`, `session`, and `web`.
  - Warn on unknown root fields by default and fail when `strict_config=true` or
    `LoadOptions::strict_unknown_fields=true`.
  - Add `config.example.json` and a load-based regression test for it.
  - Add config tests, config bench, docs, release note, and history.
- Out of scope:
  - JSON Schema generation under `docs/generated/config.schema.json`.
  - Secret encryption / decryption and rotation.
  - Full channel/team/hook/memory/automation typed config models.
  - Bootstrap CLI flags and runtime assembly.
  - Persisting config metadata into SQLite.

## Context

- Relevant docs:
  - `docs/ARCHITECTURE.md`
  - `docs/design-docs/secrets-and-state.md`
  - `docs/design-docs/module-boundaries.md`
  - `docs/rules/error-handling.md`
  - `docs/rules/libraries.md`
  - `docs/rules/testing-and-bench.md`
  - `docs/rules/docs-in-sync.md`
- Relevant code paths:
  - `xmake/{packages,targets,tests,bench}.lua`, `xmake-requires.lock`
  - New `include/oran/config/*`, `src/oran-config/*`
  - New `tests/config/*`, `bench/config/*`
  - New `config.example.json`
- Constraints:
  - Public APIs return `core::Result<T>` only.
  - Public headers do not include `<nlohmann/json.hpp>`.
  - Heavy JSON parsing stays in `.cpp` files.
  - This slice does not implement secret crypto despite documenting future secret
    behavior.
- Compile-budget impact:
  - `oran-config` has a 1.0 s median / 2.0 s p95 / 2.5 s hard cap per TU. JSON is
    confined to one implementation file.

## Risks

- Risk: Config docs promise generated JSON Schema and secret encryption before those
  exist. Mitigation: update docs with current-slice status and explicit future slices.
- Risk: Environment substitution accidentally hides missing variables. Mitigation:
  `${VAR}` is an error; only `${VAR:-default}` provides fallback.
- Risk: Public config surface grows too broad too early. Mitigation: parse only fields
  needed by bootstrap/provider routing soon; leave complex sections as future typed
  models.

## Milestones

1. Add active plan and build/package wiring.
2. Implement public API, parser, file loader, and environment substitution.
3. Add `config.example.json`, tests, and parse-vs-file-load bench.
4. Update production docs, quality score, release notes, and history.
5. Run validation gates, review generated files, and move the plan to completed.

## Validation

- Commands:
  - `make ci`
  - `xmake f -m release -y`
  - `xmake build oran-config`
  - `xmake build orangutan`
  - `xmake run test-config`
  - `xmake test`
  - `xmake run bench-config`
  - `xmake f -m release --analyze=y`
  - `xmake build -r oran-config`
  - `xmake f -m release --analyze=n`
  - `scripts/check-lib-parity.sh`
  - `git diff --check`
- Manual checks:
  - `config.example.json` loads through the production parser.
  - Public docs match shipped signatures.
  - Public config headers do not include `<nlohmann/json.hpp>`.
- Observability checks: none yet; `oran-log` is not implemented.
- Bench comparison:
  - `bench/config` compares parsing an in-memory config string vs. loading and parsing
    `config.example.json` from disk.

## Progress Log

- [x] Confirm next phase as `oran-config` foundation.
- [x] Add build/package wiring and public API.
- [x] Implement parser and environment substitution.
- [x] Add tests and bench.
- [x] Update docs that this slice invalidates in the same PR
      (`docs/rules/docs-in-sync.md`).
- [x] Run validation and record results.
- [x] Update `docs/QUALITY_SCORE.md`.
- [x] Write history entry.
- [x] Add release note.
- [x] Move this plan to `docs/exec-plans/completed/` before finish.

## Decision Log

- 2026-05-15: Start config with typed bootstrap-adjacent fields only. Rationale: broad
  channel/team/hook/memory config structs would create a schema without consumers.
- 2026-05-15: Keep secret encryption out of this slice. Rationale: the loader can mark
  and preserve secret references later, but crypto needs a separate focused slice.

## Linked Artifacts

- Related design doc: `docs/design-docs/secrets-and-state.md`
- Related product spec: `docs/product-specs/0001-core-react-loop.md`
- PRs:
- History entry: `docs/histories/2026-05/20260515-2245-oran-config-foundation.md`
- Release note: `docs/releases/feature-release-notes.md`

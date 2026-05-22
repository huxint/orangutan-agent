## [2026-05-22 14:08] | Task: tool registry schema validation

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: none — small deep-review P0 follow-up from `docs/STATUS.md`

### User Query

> Understand the current project architecture, goals, and progress, then continue implementation.

### Changes Overview

- Areas: `oran-tool`, bootstrap version, docs.
- Key actions: `Registry::add` now validates `input_schema_json` before insertion, rejects malformed schemas with `Error::invalid_argument` carrying `tool` and `schema_path`, and keeps heavy JSON parsing isolated in `src/oran-tool/schema_validation.cpp`. `xmake run orangutan` now reports slice 35.

### Design Intent

The deep-review P0 asked for schema validation at registration. This slice deliberately implements a lightweight sanity check rather than a full JSON Schema engine: it catches empty / unparseable schemas, non-object top-level schemas, and malformed common keywords (`type`, `properties`, `required`, `additionalProperties`, `enum`, `minimum`, `maximum`) without adding a dependency. The validator lives in its own TU and private header so `registry.cpp` does not pick up `nlohmann/json.hpp` compile cost.

### Files Modified

- `include/oran/tool/registry.hpp`
- `include/oran/core/tool_def.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-tool/_impl/schema_validation.hpp`
- `src/oran-tool/schema_validation.cpp`
- `src/oran-tool/registry.cpp`
- `tests/tool/test_registry.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/tool-runtime.md` — records slice 35 registration validation behavior.
- `docs/product-specs/0002-tool-registry.md` — expands the `Registry::add` acceptance criterion.
- `docs/ARCHITECTURE.md` — updates the `oran-tool` inventory and slice-status summary.
- `docs/QUALITY_SCORE.md` — updates `oran-tool` test counts and removes schema validation from next steps.
- `docs/STATUS.md` — bumps slice/history pointer, updates test counts, and refreshes remaining P0 backlog.
- `docs/exec-plans/tech-debt-tracker.md` — closes the JSON Schema validation P0 bullet.
- `docs/releases/feature-release-notes.md` — adds the slice 35 release-note row.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake run test-tool`
- Tests added/changed: added two `Registry::add` cases for invalid JSON and malformed `required`.
- Bench impact: no new bench; registration is not on the dispatch hot path.
- Compile-budget delta: JSON validation is isolated in a new implementation TU to avoid adding `nlohmann/json.hpp` to `registry.cpp`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: remaining deep-review P0 is transparent hashing on `Registry::entries_`.
- Linked release note: `docs/releases/feature-release-notes.md`

# `oran-core` — `ToolDef` Slice

## Goal

Land the small follow-up `oran-core` data slice that closes the conversation-types
surface needed by `oran-tool`, `oran-agent`, and `oran-provider`: the canonical
declaration of a tool (`ToolDef`). The previous slice deferred this explicitly
(see decision log of `2026-05-16-oran-core-message.md` — "core::ToolDef lives in
a later slice with the tool registry"). The actual runtime ownership stays with
`oran-tool` when that library lands; this slice only owns the *type*.

## Scope

- In scope:
  - `include/oran/core/tool_def.hpp` and `src/oran-core/tool_def.cpp`:
    `core::ToolDef { name, description, input_schema_json }` with member-wise
    equality, `noexcept` move semantics by default, and a static
    `ToolDef::with_no_input(std::string name, std::string description)`
    convenience builder that fills `input_schema_json` with a minimal
    `{"type":"object","properties":{}}` schema.
  - Test bucket: `tests/core/test_tool_def.cpp` — name/description fields,
    `with_no_input` schema, member-wise equality and inequality.
  - Bench: extend `bench/core/scenarios/message.cpp` *neighbour* with a small
    `tool_def_construct` scenario in `bench/core/scenarios/tool_def.cpp` that
    compares the typical pre-filled aggregate-init build of `ToolDef` against
    the `with_no_input` helper path so future call sites can pick with eyes
    open.
  - Docs: `docs/ARCHITECTURE.md` inventory + slice-status,
    `docs/QUALITY_SCORE.md` test/bench rows, `bench/core/README.md`,
    `docs/releases/feature-release-notes.md`, and a history entry.
- Out of scope:
  - `oran-tool::Registry` and runtime dispatch.
  - JSON Schema validation (lives in `oran-provider` adapters and `oran-tool`).
  - Wiring `ToolDef` into `oran-provider` request shaping.
  - Rewiring `ToolUseContent::input_json` to validate against any `ToolDef`.

## Context

- Relevant docs:
  - `docs/ARCHITECTURE.md` (lists `ToolDef` as a planned `oran-core` type).
  - `docs/design-docs/module-boundaries.md` (`ToolDef`, `ToolUse`, `ToolResult`
    explicitly belong in core).
  - `docs/design-docs/tool-runtime.md` (consumer side).
  - `docs/rules/critical-rules.md` (C6 — no JSON in public headers; C7 —
    `explicit` ctors; C17 — modern stdlib).
- Relevant code paths:
  - `include/oran/core/content.hpp` — existing tool-use / tool-result Content
    alternatives use opaque JSON; `ToolDef` follows the same opaque-string rule.
  - `src/oran-core/`, `tests/core/`, `bench/core/`.
- Constraints:
  - Pure stdlib; no new third-party packages.
  - Public header `<string>` only (the absolute minimum).
  - Schema JSON stays opaque — no nlohmann include in any new public header.
- Compile-budget impact (if any):
  - One small public header + one implementation TU + one test TU + one bench
    scenario TU. All inside the `oran-core` ≤ 1.5 s per-TU budget.

## Risks

- Risk: callers conclude that constructing `ToolDef::with_no_input` is the
  blessed path for parameter-less tools and then never reach for richer
  schemas. Mitigation: keep the helper deliberately narrow (no overloads, no
  fluent builder) and document it as a fixture/test convenience.
- Risk: defaulted equality compares full schema strings. Mitigation: schema
  is `std::string`, so default equality is fine; this matches how `Content`
  alternatives already behave for JSON payloads.

## Milestones

1. Add active plan, settle API.
2. Implement `ToolDef` header + TU.
3. Add focused tests.
4. Add bench scenario + register.
5. Update docs/history/release notes.
6. Run validation and move plan to `completed/`.

## Validation

- Commands:
  - `xmake build test-core && xmake run test-core`
  - `xmake build bench-core && xmake run bench-core`
  - `xmake test`
  - `xmake build orangutan`
  - `git diff --check`
- Manual checks:
  - Public header pulls in `<string>` only.
  - No nlohmann include anywhere in the new files.
- Observability checks:
  - None (pure value type).
- Bench comparison (if perf-relevant):
  - `core.tool_def_aggregate_init` vs. `core.tool_def_with_no_input` —
    documents the cost of the helper path vs. the bare aggregate.

## Progress Log

- [x] Confirm scope and constraints.
- [x] Implement `ToolDef`.
- [x] Add tests.
- [x] Add bench scenario.
- [x] Update docs.
- [x] Run validation.
- [x] Write history entry.
- [x] Add release note.
- [x] Move plan to `completed/`.

## Decision Log

- 2026-05-16: keep `input_schema_json` opaque. Rationale: rule C6 forbids
  nlohmann in `oran-core` public headers; JSON Schema validation is the job
  of `oran-tool` and `oran-provider` adapters once they land.

## Linked Artifacts

- Related design doc: `docs/design-docs/module-boundaries.md`,
  `docs/design-docs/tool-runtime.md`.
- Related product spec: `docs/product-specs/0002-tool-registry.md`.
- PRs: local change.
- History entry: `docs/histories/2026-05/<timestamp>-oran-core-tooldef.md`
- Release note: `docs/releases/feature-release-notes.md`

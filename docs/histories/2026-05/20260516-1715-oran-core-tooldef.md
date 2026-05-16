## [2026-05-16 17:15] | Task: `oran-core` `ToolDef` slice

### Execution Context

- Agent: Claude (Opus 4.7, fast mode, max effort)
- Base model: claude-opus-4-7
- Runtime: local xmake release build, GCC 16.1 baseline
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-core-tooldef.md`

### User Query

> 详细了解项目目标，查看当前项目进度, 推进项目代码的实现.

### Changes Overview

- Areas:
  - `oran-core` public API: tool declaration value type.
  - `oran-core` tests + bench bucket.
  - Architecture/quality/release-notes docs.
- Key actions:
  - Added `core::ToolDef` (`name`, `description`, `input_schema_json`) with
    member-wise equality and a `ToolDef::with_no_input(name, description)`
    fixture helper that fills a minimal
    `{"type":"object","properties":{},"additionalProperties":false}` schema.
  - Test: `tests/core/test_tool_def.cpp` covers aggregate-init shape, the
    helper's schema string, member-wise equality, and move construction.
  - Bench: `bench/core/scenarios/tool_def.cpp` registers
    `core.tool_def_aggregate_init` vs. `core.tool_def_with_no_input` under
    a new `bench-core/tool_def` sub-bench in `bench/core/main.cpp`.

### Design Intent

`docs/design-docs/module-boundaries.md` and the architecture inventory both
list `ToolDef` as an `oran-core` type because the agent loop, the provider
adapter, and the tool registry all need to mention it. The previous
conversation-types slice deferred `ToolDef` explicitly (decision log of
`2026-05-16-oran-core-message.md`) to keep that slice within the C14 size
guideline; this is the small follow-up.

The JSON Schema stays opaque (`std::string`) so `oran-core` honours rule C6
(no nlohmann in public headers). Validation belongs to `oran-tool` and
`oran-provider` adapters once they land.

The bench result on this host — `core.tool_def_aggregate_init` ~12.9 ns vs.
`core.tool_def_with_no_input` ~18.2 ns — documents that the helper pays one
extra short-string move plus a function-call frame over a bare aggregate.
Fixture and bootstrap call sites can pick with eyes open; production code is
expected to construct `ToolDef{...}` with a real schema directly.

### Files Modified

- `include/oran/core/tool_def.hpp`
- `src/oran-core/tool_def.cpp`
- `tests/core/test_tool_def.cpp`
- `bench/core/main.cpp`
- `bench/core/scenarios/tool_def.cpp`
- `bench/core/README.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`
- `docs/exec-plans/active/2026-05-16-oran-core-tooldef.md` (moved to
  `completed/` at the end of this slice)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — `oran-core` inventory now lists `ToolDef` as
  implemented; slice-status note records the new surface.
- `docs/QUALITY_SCORE.md` — test framework row updated to 37 cases / 223
  assertions for `oran-core`; bench harness row now mentions the `ToolDef`
  aggregate-vs-helper scenarios.
- `docs/releases/feature-release-notes.md` — added the `core-tooldef` row
  with the helper's schema, design intent, and bench numbers.
- `bench/core/README.md` — added the tool_def scenarios row describing the
  aggregate-init vs. `with_no_input` A/B.

### Validation

- Commands run:
  ```sh
  xmake build test-core
  xmake run test-core
  xmake build bench-core
  xmake run bench-core
  xmake build orangutan
  git diff --check
  ```
- Tests added/changed:
  - `tests/core/test_tool_def.cpp`: aggregate-init field shape, helper schema
    contents, member-wise equality/inequality across every field, and move
    construction preserving the schema string.
  - `tests/core` total is now 37 cases / 223 assertions (was 33 / 209).
- Bench impact:
  - `bench/core/scenarios/tool_def.cpp` adds the `bench-core/tool_def`
    sub-bench.
  - Local `xmake run bench-core` tool_def results:
    - `core.tool_def_aggregate_init`: ~12.9 ns per build.
    - `core.tool_def_with_no_input`: ~18.2 ns per build.
- Compile-budget delta:
  - One small public header added (`<string>` only). One new implementation
    TU. Both stay under the `oran-core` ≤ 1.5 s per-TU budget.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`

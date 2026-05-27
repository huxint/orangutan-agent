## [2026-05-27 21:35] | Task: slice 115 — extract `tool::detail::parse_input_object` + `require_string_field`

### Execution Context

- Agent: Claude Opus 4.7
- Base model: claude-opus-4-7
- Runtime: Claude Code (single-session implementation)
- Linked plan: none — small P1 cleanup slice; the tech-debt row is
  `review/deep-2026-05-21 → P1` (extract `tool::parse_input<T>` helper).

### User Query

> 深度了解项目架构，了解当前项目实现进度, 继续推进项目代码实现, 一定需要先读懂文档再实现!
> (Deeply understand the project architecture, then continue advancing
> implementation; must read docs first.)

The agent was asked to pick the next slice direction; the user delegated the
choice. After surveying `STATUS.md`, the spec dependency graph
(0013 → 0011 + 0012 → 0014 → 0016 → 0017 → 0015 → 0018), and
`exec-plans/tech-debt-tracker.md`, the agent selected the `tool::parse_input`
P1 row as the highest-confidence single-concern slice with a clear acceptance
criterion. The next two structural items (ToolScheduler v1 first slice and the
deferred singleflight regression test) both require multi-slice infrastructure
investments and were deferred.

### Changes Overview

- Areas: `oran-tool` (built-in input parsing).
- Key actions:
  - Add `src/oran-tool/_impl/parse_input.hpp` exporting
    `detail::parse_input_object(input_json, tool_name)` and
    `detail::require_string_field(input, tool_name, field)` — the two
    operations every built-in tool needs before its tool-specific options
    parsing.
  - Refactor `file.read`, `file.write`, `file.edit`, `file.delete`,
    `file.search`, `directory.list`, and `tool.search` to consume the helpers
    instead of inlining the try/catch JSON parse + `is_object` check + required
    string field pattern.
  - Standardise the error vocabulary across the catalog: `"<tool>: input is not
    valid JSON"` (with `detail`), `"<tool>: input must be a JSON object"`, and
    `"<tool>: input must include a string `<field>` field"`. The three built-ins
    that previously combined the object + path checks (`file.read`,
    `file.delete`, `directory.list`) now report the two failure modes as
    separate, distinct errors; existing tests assert
    `error.kind()`, not the message text, so the behaviour change is invisible
    to all current consumers.

### Design Intent

The deep review on 2026-05-21 surfaced this as a P1 cleanup: ~120 lines of
near-identical JSON parsing boilerplate were duplicated across six built-in
tools, and the seventh (`tool.search`) had carved out its own private
`parse_json` helper that did almost the same thing. Each new built-in
(`code.symbols` / `code.references` from the same P1 list, plus the
already-tracked unified `delete` and recursive `directory` work in the future
tool-design memo) would have to copy the same boilerplate.

The minimal absorption is two free functions: one for parse + `is_object` and
one for the most common required-string-field shape. The signatures take the
tool name as a `string_view` so the canonical error messages carry the same
`<tool>:` prefix the catalog has always emitted; this means an LLM consuming
the error can still attribute the failure to the right tool without changing
how it parses error text.

The helpers live in `src/oran-tool/_impl/parse_input.hpp` rather than the
public `<oran/tool/...>` headers because they expose `nlohmann::json` in their
return type — `critical-rules.md#C6` forbids nlohmann/json in public headers,
and there is no caller outside `oran-tool` that needs them today. Direct unit
tests under `tests/tool/test_parse_input.cpp` include the private header via
relative path, matching how the in-repo precedent (no precedent yet — this is
the first direct-test of an `_impl` helper, and is the right amount of
specification for a contract three more built-ins will depend on).

Net delta: 166 lines removed from the seven built-ins, 115 lines re-added in
the simpler helper-based form, 85 lines of new shared infrastructure (helper
header + impl), 98 lines of direct unit tests. Test count: `test-tool`
goes from 178 cases / 1838 assertions to **185 / 1866** (+7 cases, +28
assertions for the new direct helper tests; the seven refactored built-ins
keep their existing test coverage unchanged).

### Files Modified

- `src/oran-tool/_impl/parse_input.hpp` — **new** private header.
- `src/oran-tool/parse_input.cpp` — **new** implementation.
- `src/oran-tool/file_read.cpp` — consume `parse_input_object` +
  `require_string_field`; drop the local try/catch JSON parse block and the
  combined `is_object` + path-string check.
- `src/oran-tool/file_write.cpp` — same; preserved
  separate validation of optional `mode` / `create_parents` /
  `expected_version` fields.
- `src/oran-tool/file_edit.cpp` — same; captures the original input path into
  `input_path` so the output `format` message keeps its
  pre-workspace-resolution value.
- `src/oran-tool/file_delete.cpp` — same.
- `src/oran-tool/file_search.cpp` — same; replaced the local
  `try`/`catch` block, the `is_object` check, and the separate path/pattern
  string-field checks with two `require_string_field` calls.
- `src/oran-tool/directory_list.cpp` — same.
- `src/oran-tool/tool_search.cpp` — replaced the local `parse_json` +
  `is_object` check with `parse_input_object`; retained the local
  `read_string_selector` because its optional-plus-non-empty contract is
  unique to selector fields and does not generalise to the file-tool family.
- `tests/tool/test_parse_input.cpp` — **new** direct unit coverage for both
  helpers (7 cases, 28 assertions).

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 115, refreshed
  `Last completed history` pointer, refreshed `test-tool` line in
  `Latest Library Surfaces` (178 / 1838 → 185 / 1866), rewrote the
  next-intended-slice paragraph to cite this slice's completion, and
  contracted the `review/deep-2026-05-21 → P1` bullet in the tech-debt row to
  remove the `parse_input<T>` item (the helper now exists).
- `docs/exec-plans/tech-debt-tracker.md` — same `review/deep-2026-05-21` row
  contraction.

### Validation

- Commands run:
  - `xmake build oran-tool` — build succeeded with the seven refactored
    callers + the new helper TU.
  - `xmake build test-tool` — built the new test_parse_input.cpp.
  - `xmake run test-tool` — **All tests passed (1866 assertions in 185 test
    cases).**
  - `xmake run test-agent` — **All tests passed (407 assertions in 26 test
    cases)** (unchanged; downstream sanity-check that the registry/dispatch
    contract has not drifted).
  - `xmake run test-bootstrap` — **All tests passed (316 assertions in 72 test
    cases)** (unchanged).
- Tests added/changed:
  - `tests/tool/test_parse_input.cpp` — 7 new direct test cases / 28
    assertions covering: malformed JSON rejection with `detail` context,
    non-object top-level rejection (array + scalar), happy-path returning
    parsed json, `require_string_field` happy path, missing-field rejection,
    non-string-field rejection, and tool-name passthrough across two distinct
    tool names.
  - No existing tool tests were modified — the refactor preserves byte-level
    error-kind contract and changes only message text for three of the seven
    tools' previously-combined "object-with-path" error, which no existing
    test asserts.
- Bench impact:
  - No new bench. The helper is on the cold per-call path; the cost is
    dominated by the existing `nlohmann::json::parse` call which has not
    changed shape.
- Compile-budget delta:
  - Not measured. The new helper TU compiles `nlohmann/json.hpp` once (it was
    already on each refactored built-in's TU); the net inclusion graph is
    unchanged.

### Follow-ups

- Tech-debt entries: the `review/deep-2026-05-21 → P1` row in
  `tech-debt-tracker.md` had four P1 items; this slice closes
  `tool::parse_input<T>`. Remaining P1 items: first `code.*` family
  (`code.symbols` / `code.references`) and MVP `oran-skill` loader. P2 items
  unchanged.
- Issues opened: none.
- Linked release note: none — internal refactor, no user-visible
  behaviour change.

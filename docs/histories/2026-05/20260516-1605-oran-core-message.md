## [2026-05-16 16:05] | Task: `oran-core` conversation types slice

### Execution Context

- Agent: Claude (Opus 4.7, fast mode, max effort)
- Base model: claude-opus-4-7
- Runtime: local xmake release build, GCC 16.1 baseline
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-core-message.md`

### User Query

> good! commit 当前的修改. 然后开始推进项目下一阶段的执行

### Changes Overview

- Areas:
  - `oran-core` public API: conversation-layer data types.
  - `oran-core` tests and bench bucket.
  - Architecture/quality/release-notes docs.
- Key actions:
  - Added `core::Role` (`user`/`assistant`/`system`/`tool`) and
    `core::StopReason` (`end_turn`/`max_tokens`/`tool_use`/`stop_sequence`/
    `cancelled`/`error`) with stable `to_string_view` mappings and
    `std::formatter` specializations.
  - Added `core::Content` as `std::variant<TextContent, ThinkingContent,
    ToolUseContent, ToolResultContent>` with member-wise equality on every
    alternative, plus `holds_text`/`holds_thinking`/`holds_tool_use`/
    `holds_tool_result` predicates and the lifetime-aware `text_view`
    helper.
  - Added `core::Message` (`role` + `std::vector<Content> blocks` +
    `std::optional<Time> created_at`) with member-wise equality and
    `Message::user_text` / `Message::assistant_text` factories for tests
    and bootstrap-side fixtures.
  - Tests: `tests/core/test_role.cpp`, `tests/core/test_stop_reason.cpp`,
    `tests/core/test_content.cpp`, `tests/core/test_message.cpp`.
  - Bench: `bench/core/scenarios/message.cpp` registers
    `core.content_visit_overloaded`, `core.content_get_if_text`, and
    `core.message_walk_blocks` under a new `bench-core/message`
    sub-bench in `bench/core/main.cpp`.

### Design Intent

`docs/design-docs/module-boundaries.md` and the architecture inventory both
list `Role`, `Content`, `Message`, and `StopReason` as `oran-core` types
because every layer above (`storage`, `memory`, `provider`, `agent`,
`channel`, `cli`) consumes the same shape. Until this slice landed, every
consumer either had to invent its own JSON-string contract (as
`SessionRepository` did) or block on the data model.

The `Content` variant alternatives keep their JSON payloads as opaque
`std::string`. This honors rule C6 (no nlohmann in public headers) and
puts the JSON encode/decode responsibility on `oran-provider` adapters
where vendor-shape translation belongs.

The bench result on this host — `core.content_visit_overloaded` ~28 ns vs.
`core.content_get_if_text` ~26 ns for a 32-block walk — documents that
`std::visit` with an `Overloaded` set is *not* paying a measurable cost over
the single-alternative shortcut. That removes a common excuse to inline
`get_if` chains in future hot paths.

### Files Modified

- `include/oran/core/role.hpp`
- `include/oran/core/stop_reason.hpp`
- `include/oran/core/content.hpp`
- `include/oran/core/message.hpp`
- `src/oran-core/role.cpp`
- `src/oran-core/stop_reason.cpp`
- `src/oran-core/content.cpp`
- `src/oran-core/message.cpp`
- `tests/core/test_role.cpp`
- `tests/core/test_stop_reason.cpp`
- `tests/core/test_content.cpp`
- `tests/core/test_message.cpp`
- `bench/core/main.cpp`
- `bench/core/scenarios/message.cpp`
- `bench/core/README.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`
- `docs/exec-plans/active/2026-05-16-oran-core-message.md` (moved to
  `completed/` at the end of this slice)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — `oran-core` inventory now lists `Role`,
  `StopReason`, `Content`, and `Message` as implemented; slice-status note
  records the new surface.
- `docs/QUALITY_SCORE.md` — test framework row updated to 33 cases / 209
  assertions for `oran-core`; bench harness row now mentions the Content
  visit-vs-get_if and message-walk scenarios.
- `docs/releases/feature-release-notes.md` — added the `core-message` row
  with new types, opaque-JSON design intent, and bench numbers.
- `bench/core/README.md` — added the message scenarios row describing the
  visit-vs-get_if A/B and the walk-blocks baseline.

### Validation

- Commands run:
  ```sh
  xmake build test-core
  xmake run test-core
  xmake build bench-core
  xmake run bench-core
  xmake test
  xmake build orangutan
  git diff --check
  make ci
  ```
- Tests added/changed:
  - `tests/core/test_role.cpp`: enum mapping, formatter, out-of-range fallback.
  - `tests/core/test_stop_reason.cpp`: enum mapping, formatter, out-of-range
    fallback.
  - `tests/core/test_content.cpp`: every `holds_*` predicate, `text_view`
    presence/absence, member-wise equality (text and tool-use), cross-
    alternative inequality, `is_error` flag in `ToolResultContent`, and a
    visitor walk that covers every alternative.
  - `tests/core/test_message.cpp`: factory shape, role/blocks/timestamp
    equality, mixed-alternative block construction.
  - `tests/core` total is now 33 cases / 209 assertions (was 17 / 150).
- Bench impact:
  - `bench/core/scenarios/message.cpp` adds the message scenarios.
  - Local `xmake run bench-core` message results:
    - `core.content_visit_overloaded`: ~28.0 ns per 32-block walk.
    - `core.content_get_if_text`: ~26.3 ns per 32-block walk.
    - `core.message_walk_blocks`: ~31.2 ns per 32-block walk.
  - Time and error scenarios are unchanged in shape; error scenarios remain
    ~69 ns / `Error`, time scenarios ~330 ns format-explicit / ~157 ns
    format-chrono / ~42 ns parse on this run.
- Compile-budget delta:
  - Four small public headers added. The heaviest addition to the public
    core surface is `<variant>` (via `content.hpp`); `<vector>` is the
    other new include via `message.hpp`. No asio / sqlite / nlohmann
    include enters the public surface.
  - Four new implementation TUs (`role.cpp`, `stop_reason.cpp`,
    `content.cpp`, `message.cpp`); each stays under the
    `docs/design-docs/module-boundaries.md` per-TU budget for
    `oran-core`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`

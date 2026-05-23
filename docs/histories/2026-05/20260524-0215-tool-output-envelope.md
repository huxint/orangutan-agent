## [2026-05-24 02:15] | Task: `oran-tool` output envelope

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - scoped spec 0014 foundation slice.

### User Query

> Continue advancing the structured tool-output work from the documented
> architecture.

### Changes Overview

- Areas: `oran-tool`, `oran-hook`, tests, bench, slice version,
  docs/status.
- Key actions:
  - Moved the tool result surface out of `registry.hpp` into
    `<oran/tool/output.hpp>`.
  - Promoted `tool::Output` from text plus error flag into the spec-0014
    envelope: required `text`, optional serialized `data_json`,
    attachment metadata, usage counters, and `is_error`.
  - Added `Output::text_only(...)` for the v1-compatible path and
    `Output::error(...)` for structured-capable error envelopes.
  - Added a hook-local `hook::ToolUsage` payload shape and copied
    successful `Output::usage` into `ToolAfterPayload::usage`.
  - Added focused envelope tests and `bench-tool` output construction
    scenarios.
  - `xmake run orangutan` now reports `2.0.0-slice60`.

### Design Intent

Spec 0014 needs a structured envelope before provider adapters and the
tool scheduler can map or cap structured tool results. This slice lands the
small public value type first while keeping the header cheap: `data_json`
is serialized bytes rather than a public `nlohmann::json`, and attachments
are metadata-only until a real producer needs blob transport.

The hook usage copy is intentionally one-way and dependency-free:
`oran-hook` does not depend on `oran-tool`, and failed dispatches still
emit empty usage so sinks can distinguish handler metrics from permission
or audit failures. Provider adapter mapping, scheduler byte caps, audit
usage fan-out, hook raw-data redaction, and built-in structured payload
migrations remain downstream under spec 0014.

### Files Modified

- `include/oran/tool/output.hpp`
- `include/oran/tool.hpp`
- `include/oran/tool/registry.hpp`
- `include/oran/hook/payload.hpp`
- `src/oran-tool/registry.cpp`
- `tests/tool/test_output.cpp`
- `tests/tool/test_registry.cpp`
- `bench/tool/scenarios/output.cpp`
- `bench/tool/main.cpp`
- `bench/tool/README.md`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/tool-runtime.md`
- `docs/design-docs/permissions-and-hooks.md`
- `docs/product-specs/0014-structured-tool-output.md`
- `docs/exec-plans/tech-debt-tracker.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 60, new history pointer, refreshed
  `oran-tool` test counts, and remaining spec-0014 follow-ups.
- `docs/ARCHITECTURE.md` and `docs/design-docs/tool-runtime.md` -
  public `tool::Output` / `ToolUsage` / `Attachment` surface and the
  remaining downstream transport work.
- `docs/design-docs/permissions-and-hooks.md` - `ToolAfterPayload`
  now carries hook-local usage metrics.
- `docs/product-specs/0014-structured-tool-output.md` - records the
  shipped base envelope while leaving adapter mapping, byte caps, audit
  fan-out, redaction, and built-in migrations future.
- `docs/exec-plans/tech-debt-tracker.md` - closes the deep-review
  P2 `tool::Output` too-small item and leaves `tool::parse_input<T>`
  open.
- `docs/releases/feature-release-notes.md` - user-visible release note.
- `docs/QUALITY_SCORE.md` - refreshed counts and tool/hook surface
  summaries.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `xmake run test-tool "[output]"` (repo wrapper ran the full
    bucket: 145 cases / 1250 assertions)
  - `xmake build bench-tool`
  - `xmake run bench-tool`
  - `xmake build orangutan`
  - `xmake run orangutan -- --prompt "slice60 smoke"`
  - `make ci`
- Tests added/changed:
  - `tests/tool/test_output.cpp` covers text-only, error,
    usage-empty, serialized data, and attachment metadata paths.
  - `tests/tool/test_registry.cpp` covers successful `Output::usage`
    propagation into `tool_after`.
- Bench impact:
  - `bench/tool/scenarios/output.cpp` adds `output.text_only` vs.
    `output.with_data_16kib`.
- Compile-budget delta:
  - The new public header uses only standard-library containers and
    chrono/optional/string/vector; JSON parsing remains in implementation
    TUs and future provider adapters.

### Follow-Ups

- Provider adapters still need to map `data_json` into each vendor
  tool-result shape.
- The scheduler still owns text/data byte caps and cap flags.
- Audit usage fan-out, raw `data_json` redaction for trusted sinks, and
  built-in structured payload migrations remain under spec 0014.

# 2026-06-04 12:07 | Task: Hook Mutation Input Redaction

## Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: codex-cli
- Linked plan: none

## User Query

> Start the next slice after reading the project architecture and current state;
> continuation requested for session `019e90c8-23b7-72b0-8b82-1a96399eaa58`.

## Changes Overview

- Areas: `oran-hook`, `oran-tool`, CLI approval docs, deep-review tracker.
- Key actions: added optional `redacted_input_json` to tool/permission hook
  payloads, made `hook::Bus` substitute that view for non-`trusted_local`
  sinks across advisory and blocking publishes, and taught `Registry::dispatch`
  to fill compact hash-and-byte-count summaries for `file.write` and
  `file.edit` inputs.

## Design Intent

The deep-review follow-up asked for default hook observers to stop receiving raw
mutation payloads for `file.write` and `file.edit`. The chosen boundary reuses
the existing `SinkKind::trusted_local` trust label from structured-output
redaction: trusted in-process observers keep the exact `input_json`, while all
default sinks receive a deterministic summary with `kind=redacted_tool_input`,
`tool_name`, the SHA-256 input hash, input byte count, and redacted string byte
counts. Malformed JSON still produces a hash-only summary so hooks can correlate
events without seeing the original bytes.

## Files Modified

- `include/oran/hook/payload.hpp`
- `include/oran/tool/registry.hpp`
- `src/oran-hook/bus.cpp`
- `src/oran-tool/_impl/input_redaction.hpp`
- `src/oran-tool/input_redaction.cpp`
- `src/oran-tool/registry.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/hook/test_bus.cpp`
- `tests/hook/test_publish_blocking.cpp`
- `tests/tool/test_registry.cpp`

## Docs Updated In This PR

- `docs/design-docs/permissions-and-hooks.md` - documents the per-sink mutation
  input redaction channel and summary fields.
- `docs/design-docs/tool-runtime.md` - documents `file.write` / `file.edit`
  hook input summaries.
- `docs/design-docs/cli-runtime.md` - clarifies that approval prompts render the
  bus-delivered payload input, which may be redacted for default sinks.
- `docs/product-specs/0014-structured-tool-output.md` - extends the hook
  redaction acceptance note from structured output to sensitive mutation input.
- `docs/product-specs/0015-blocking-hook-decisions.md` - documents that blocking
  publishes use the same input redaction boundary.
- `docs/ARCHITECTURE.md` - refreshes `oran-tool` / `oran-hook` inventory rows.
- `docs/QUALITY_SCORE.md` - refreshes focused test counts and hook/tool notes.
- `docs/releases/feature-release-notes.md` - adds the slice 152 release note.
- `docs/exec-plans/tech-debt-tracker.md` - closes the deep-review follow-up item.
- `docs/STATUS.md` - bumps the snapshot to slice 152.

## Validation

- Commands run:
  - `xmake build test-hook`
  - `xmake build test-tool`
  - `xmake run test-hook`
  - `xmake run test-tool`
  - `xmake build orangutan`
  - `xmake run orangutan -- --prompt "2+2"`
  - `make ci`
  - `xmake test`
- Tests added/changed: hook bus redaction tests for advisory and blocking
  delivery; registry tests for `file.write` / `file.edit` before/dispatched/after
  input redaction plus `tool_error` and `permission_ask_rendered`.
- Bench impact: no benchmark added; this is a correctness/privacy boundary on
  hook payload copies, not a hot-path alternative selection.
- Compile-budget delta: one small `oran-tool` TU was added to keep JSON parsing
  out of public headers and away from the main dispatch implementation.

## Follow-ups

- Issues opened: none.
- Tech-debt entries: the 2026-05-21 deep-review follow-up row now has this item
  closed; remaining entries are CI xmake wiring and atomic-write durability.
- Linked release note: `docs/releases/feature-release-notes.md` entry
  `hook-mutation-input-redaction`.

## [2026-06-07 00:39] | Task: memory startup decay hooks

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: CLI coding session
- Linked plan: none

### User Query

> Continue the most valuable current implementation slice with docs-first
> orientation, avoid bench-only work by default, keep docs/status/history in
> sync, validate the result, and commit with a compliant message.

### Changes Overview

- Areas: bootstrap runtime assembly, hook payloads, long-term memory startup
  retention observability.
- Key actions: added typed `MemoryDecayPayload`, added build-only
  `RuntimeAssemblyOptions::startup_hook_bindings`, published advisory
  `memory_decay` after a successful configured-route startup decay pass, unbound
  startup observers before returning the assembly, and added focused hook plus
  bootstrap regression coverage.

### Design Intent

The slice makes the shipped startup retention pass observable without claiming
periodic retention scheduling is complete. `RuntimeAssembly::build` is
synchronous and runs startup producers before callers can bind to the returned
bus, so startup observers need an explicit build-time binding surface. The
payload is metadata-only: source, scope, policy inputs, shadowed count, and
timing are enough for observability, while decayed record content stays out of
both default and trusted-local hook payloads.

### Files Modified

- `include/oran/hook/payload.hpp`
- `include/oran/hook/event.hpp`
- `include/oran/bootstrap/runtime_assembly.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-bootstrap/runtime_assembly.cpp`
- `tests/hook/test_bus.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped the current slice, latest history, next-slice note,
  and focused hook/bootstrap counts.
- `docs/ARCHITECTURE.md` — updated hook, memory, and bootstrap inventory rows for
  startup decay hook publishing.
- `docs/QUALITY_SCORE.md` — refreshed test counts and the bootstrap/memory/hook
  quality rows for slice 186.
- `docs/design-docs/bootstrap-runtime.md` — documented startup hook bindings,
  null-sink rejection, startup `memory_decay`, and unbind-before-return behavior.
- `docs/design-docs/permissions-and-hooks.md` — documented
  `MemoryDecayPayload`, its metadata-only shape, and the startup producer.
- `docs/design-docs/memory-system.md` — documented startup retention hook
  observability while leaving periodic automation publishing downstream.
- `docs/product-specs/0005-memory-system.md` — updated acceptance criteria and
  validation notes for startup retention hook observability.
- `docs/releases/feature-release-notes.md` — added the user-visible slice 186
  release note.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-hook`
  - `xmake run test-hook`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed: added hook-bus coverage for advisory
  `MemoryDecayPayload` delivery and bootstrap runtime-assembly coverage for null
  startup bindings, startup decay payload metadata, and startup observer
  unbinding.
- Bench impact: none; this is a startup observability path, not a runtime
  performance tradeoff.
- Compile-budget delta: no new public heavy includes; the new public surface adds
  only `std::vector` plus hook forward declarations.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#memory-startup-decay-hooks`.
- Downstream work: periodic automation retention cadence and periodic decay
  producer ownership, semantic/external embeddings, and optional `MEMORY.md`
  mirror.

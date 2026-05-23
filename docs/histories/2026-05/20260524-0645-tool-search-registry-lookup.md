## [2026-05-24 06:45] | Task: tool.search registry lookup

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local xmake / GCC 16.1 C++26`
- Linked plan: none — this was a narrow slice under `docs/STATUS.md`.

### User Query

Continue implementing Orangutan v2 one slice at a time, with project docs read
first and docs kept in sync with code.

### Changes Overview

- Areas: `oran-tool` built-ins, spec 0016 deferred-tool lookup prework,
  bootstrap version reporting, tests, docs.
- Key actions: added non-deferred `tool.search` as a capability-free registry
  metadata lookup; wired it into `register_builtins`; and covered metadata
  lookup, late-registered deferred tools, registry moves, and malformed
  selectors in `test-tool`.

### Design Intent

Spec 0016 needs two different pieces: a callable lookup tool and per-session
promotion state. This slice deliberately ships only the registry-owned lookup
primitive because `oran-agent::SessionState` and the prompt builder do not
exist yet. The handler reads `ctx.registry`, which `Registry::dispatch` sets
to the currently dispatching registry and restores on exit, then searches
`Registry::catalog()` at dispatch time. That makes tools registered after
`tool.search` discoverable and avoids storing a self-reference inside a
movable `Registry`, so lookup remains correct if a registry value is moved
after registration. Promotion remains future agent state, not hidden mutable
state inside `tool::Registry`.

### Files Modified

- `include/oran/tool/builtins.hpp`
- `include/oran/tool/registry.hpp`
- `src/oran-tool/builtins.cpp`
- `src/oran-tool/registry.cpp`
- `src/oran-tool/tool_search.cpp`
- `tests/tool/test_registry.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/agent-platform.md`
- `docs/design-docs/tool-runtime.md`
- `docs/product-specs/0002-tool-registry.md`
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md`
- `docs/QUALITY_SCORE.md`
- `docs/rules/prompt-design.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 68 snapshot, shipped lookup primitive, and remaining
  0016 prompt/session work.
- `docs/ARCHITECTURE.md` — `oran-tool` inventory now includes `tool.search`.
- `docs/design-docs/tool-runtime.md` — deferred-tool status now distinguishes
  shipped lookup from future promotion state.
- `docs/design-docs/agent-platform.md` — prompt-builder wording now uses the
  shipped `tool.search` wire name.
- `docs/product-specs/0002-tool-registry.md` — acceptance wording now uses the
  shipped `tool.search` wire name.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — records the
  implemented registry lookup and the future promotion side effect.
- `docs/QUALITY_SCORE.md` — updated tool test counts and tool-registry status.
- `docs/rules/prompt-design.md` — deferred-tool promotion wording now uses the
  shipped `tool.search` wire name.
- `docs/releases/feature-release-notes.md` — added the user-visible slice note.

### Validation

- Commands run:
  - `xmake build test-tool`
  - `./build/linux/x86_64/release/test-tool "[unit][tool][tool_search]" --reporter=console --verbosity=normal` — 5 cases / 63 assertions
  - `./build/linux/x86_64/release/test-tool --reporter=console --verbosity=normal` — 166 cases / 1588 assertions
  - `make check-docs`
  - `xmake build orangutan`
  - `xmake run orangutan --prompt smoke`
  - `xmake test`
  - `make ci`
- Tests added/changed: five `tool.search` registry tests; `register_builtins`
  now asserts the aggregate catalog includes `tool.search`.
- Bench impact: no new bench; lookup is a metadata scan with no new hot-path
  tradeoff, and the existing catalog-renderer/registry benches still cover the
  surrounding surfaces.
- Compile-budget delta: one small `src/oran-tool/tool_search.cpp` TU; full
  budget gate runs in final validation.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`

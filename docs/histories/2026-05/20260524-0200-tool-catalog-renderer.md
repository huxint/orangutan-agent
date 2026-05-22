## [2026-05-24 02:00] | Task: `oran-tool` deterministic catalog renderer

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - scoped prework for specs 0012 and 0016.

### User Query

> Continue advancing implementation versions from the documented architecture.

### Changes Overview

- Areas: `oran-core`, `oran-tool`, tests, bench, slice version, docs/status.
- Key actions:
  - Extended `core::ToolDef` with the documented `deferred` and
    `category` metadata.
  - Added public `tool::CatalogRenderer`,
    `ToolCatalogRenderOptions`, `RenderedToolCatalog`, and
    `ToolCatalogCacheStats`.
  - Rendered catalog snapshots deterministically by tool name, with
    active tools as canonical full-schema blocks and deferred tools as
    compact name/description rows.
  - Kept JSON parsing and canonicalisation in `src/oran-tool/catalog.cpp`
    and hid the bounded rendered-block cache behind a pimpl so the public
    header stays nlohmann-free and does not expose `BoundedCache`.
  - Set category metadata on the shipped filesystem built-ins.
  - Added `bench-tool` cold-vs-hot catalog rendering coverage.
  - `xmake run orangutan` now reports `2.0.0-slice59`.

### Design Intent

The prompt builder does not exist yet, but the tool catalog already has to
stop being an order-dependent pile of raw `ToolDef` values. This slice puts
the deterministic renderer in `oran-tool`, where the registry snapshot and
schema rendering rules live, without creating `oran-prompt` early or
inventing agent-session promotion state before there is an agent loop.

The rendered-block cache is deliberately aggregate-only from the outside:
stats expose hits, misses, evictions, and occupancy, but not schemas, tool
names, or cache keys. Hash collisions are guarded by a private fingerprint
before a cached block is reused. The full-schema block key excludes the
`deferred` placement bit because that bit affects section membership, not
the rendered block bytes; setting `max_cached_blocks = 0` disables
memoisation instead of permitting unbounded state.

### Files Modified

- `include/oran/core/tool_def.hpp`
- `include/oran/tool/catalog.hpp`
- `include/oran/tool.hpp`
- `src/oran-core/tool_def.cpp`
- `src/oran-tool/catalog.cpp`
- `src/oran-tool/file_read.cpp`
- `src/oran-tool/file_write.cpp`
- `src/oran-tool/file_edit.cpp`
- `src/oran-tool/file_search.cpp`
- `src/oran-tool/file_delete.cpp`
- `src/oran-tool/directory_list.cpp`
- `tests/core/test_tool_def.cpp`
- `tests/tool/test_registry.cpp`
- `bench/tool/scenarios/catalog.cpp`
- `bench/tool/main.cpp`
- `bench/tool/README.md`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/tool-runtime.md`
- `docs/product-specs/0012-tool-scheduler-and-state.md`
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 59, new history pointer, refreshed
  `oran-core` and `oran-tool` test counts, and next-intended-slice
  narrative.
- `docs/ARCHITECTURE.md` and `docs/design-docs/tool-runtime.md` -
  public surfaces document `ToolDef` metadata and `CatalogRenderer`.
- `docs/product-specs/0012-tool-scheduler-and-state.md` - marks the
  tool-catalog rendered-block cache as shipped in `oran-tool`.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` - records
  the shipped renderer while leaving `oran-prompt`, active-tool config,
  `tool.search`, and promotion state future.
- `docs/releases/feature-release-notes.md` - user-visible release note.
- `docs/QUALITY_SCORE.md` - refreshed counts and tool-registry state.

### Validation

- Commands run:
  - `xmake build oran-core`
  - `xmake build oran-tool`
  - `xmake build test-core`
  - `xmake build test-tool`
  - `xmake run test-core "[tool_def]"`
  - `xmake run test-tool "[catalog]"`
  - `xmake build bench-tool`
- Tests added/changed:
  - `tests/core/test_tool_def.cpp` covers `deferred` and `category`.
  - `tests/tool/test_registry.cpp` adds four `[catalog]` regressions
    covering deterministic block rendering, active/deferred separation,
    renderer-version cache keys, disabled-cache behaviour, and cache
    hit/miss stats.
  - Focused `test-core` reports 69 cases / 450 assertions.
  - Focused `test-tool` reports 140 cases / 1209 assertions.
- Bench impact:
  - `bench/tool/scenarios/catalog.cpp` adds
    `catalog.render_cold_32_tools` vs. `catalog.render_hot_32_tools`.
- Compile-budget delta:
  - Public `oran-tool` header adds a pimpl-based renderer surface.
    Heavy JSON and `BoundedCache` stay in `src/oran-tool/catalog.cpp`.

### Follow-Ups

- `oran-prompt` still needs the full `prompt::Builder`, active-tool
  config, prompt sections, and prompt-cache stability bench.
- `tool.search` and per-session promotion state remain future
  `oran-agent` work.

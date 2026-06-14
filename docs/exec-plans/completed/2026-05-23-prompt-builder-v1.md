# Prompt Builder v1

## Goal

Land the first `oran-prompt` implementation that `oran-agent` can call before the
fake-provider loop ships: a deterministic prompt builder with cache sections,
active/deferred tool catalog rendering driven by `runtime.prompt.active_tools`,
stable prefix hashing, and enough tests/bench coverage to guard the prompt-cache
invariants from `docs/rules/prompt-design.md`.

## Scope

- In scope:
  - Add the `oran-prompt` library target plus public headers, tests, and bench bucket.
  - Define `prompt::CacheSection`, `prompt::RenderedPrompt`, and `prompt::Builder`.
  - Consume `config::PromptActiveToolsConfig` and `tool::CatalogRenderer` so section
    2 contains the configured active set and section 3 contains deferred/index rows.
  - Compute stable section content hashes, prefix hash, prefix byte count, and the
    single breakpoint before the conversation tail.
  - Keep the first slice independent of `oran-agent` / provider adapters.
- Out of scope:
  - Per-session promotion state from `ToolSearch` results.
  - Memory and skill body renderers beyond empty/stub section inputs.
  - Provider adapter cache mapping and fake-provider agent-loop scenarios.
  - Generated prompt-schema or real prompt text polish beyond deterministic section
    structure.

## Context

- Relevant docs:
  - `docs/product-specs/0016-prompt-and-tool-catalog-cache.md`
  - `docs/product-specs/0017-fake-provider-first-agent-loop.md`
  - `docs/design-docs/agent-platform.md`
  - `docs/design-docs/tool-runtime.md` "Catalog Renderer"
  - `docs/design-docs/api-portability.md` "Caching"
  - `docs/rules/prompt-design.md`
  - `docs/design-docs/module-boundaries.md`
- Relevant code paths:
  - `include/oran/config/config.hpp` (`PromptActiveToolsConfig`)
  - `include/oran/tool/catalog.hpp` / `src/oran-tool/catalog.cpp`
  - `include/oran/core/{message,content,tool_def}.hpp`
  - `xmake/{targets,tests,bench}.lua`
- Constraints:
  - Public prompt headers stay nlohmann-free and avoid heavy includes.
  - `oran-prompt` may depend on `oran-core`, `oran-async`, `oran-config`, and
    `oran-tool`; any same-layer dependency on `oran-tool` must be documented and
    allowed in `scripts/check-deps.sh`.
  - No clocks, request IDs, or per-call counters enter sections 1-6.
  - Cache-version changes must invalidate `prefix_hash` even when content bytes are
    unchanged.
- Compile-budget impact:
  - New `oran-prompt` TUs use the existing prompt budget row
    (`median=1.5s`, `p95=3.0s`, `hard_cap=3.5s`) from `compile_budget.json`.
  - No new third-party package is expected.

## Risks

- Risk: the first builder bakes in a shape the fake-provider loop cannot consume.
  Mitigation: keep inputs in terms of existing `core::Message`, `core::ToolDef`, and
  config/tool catalog types named by specs 0016/0017.
- Risk: catalog rendering duplicates logic already owned by `oran-tool`.
  Mitigation: `oran-prompt` delegates tool block rendering to `tool::CatalogRenderer`
  and only owns active/deferred selection plus section assembly.
- Risk: prompt hashes drift from hidden dynamic data.
  Mitigation: section rendering is pure over `Builder::Inputs`; tests compare
  identical prefixes across different conversation tails.
- Risk: the new library creates dependency drift.
  Mitigation: update `ARCHITECTURE.md`, `BUILD_SYSTEM.md`, and `scripts/check-deps.sh`
  in the same slice and run `make ci`.

## Milestones

1. `oran-prompt` builder skeleton: sections 1-7, active/deferred tool catalog from
   `runtime.prompt.active_tools`, stable hashes, tests, and a small bench.
2. Promotion state: session-local promoted deferred tools with LRU/TTL bound from spec
   0012, consumed by the builder on the *next* turn.
3. Prompt-cache stability bench: `bench/oran-agent/prompt_cache_hit_rate.cpp` or an
   equivalent prompt-owned precursor that validates prefix stability across fixtures.
4. Agent-loop handoff: wire `oran-agent` fake-provider v1 to call the builder.

## Validation

- Commands:
  - `xmake build oran-prompt test-prompt bench-prompt`
  - `xmake run test-prompt`
  - `xmake run bench-prompt`
  - `xmake build orangutan`
  - `xmake test`
  - `make ci`
- Manual checks:
  - Confirm section order and breakpoint match `docs/rules/prompt-design.md`.
  - Confirm explicit active allowlists move every other registered tool to section 3.
  - Confirm missing explicit tool names fail in the builder rather than config.
- Observability checks:
  - First slice exposes prefix hash and section hashes in `RenderedPrompt`; runtime
    trace emission remains spec 0018 / agent-loop work.
- Bench comparison:
  - First slice measures default active set vs explicit active subset. More complete
    cache-hit-rate benches land when promotion or agent fixtures exist.

## Progress Log

- [x] 2026-05-23: confirm scope and constraints from STATUS, specs 0016/0017,
  prompt-design, agent-platform, api-portability, tool-runtime, module-boundaries,
  and compile-budget.
- [x] Implement milestone 1.
- [x] Update same-slice docs per `docs/rules/docs-in-sync.md`.
- [x] Run full validation and record results.
- [x] Update `docs/QUALITY_SCORE.md`.
- [x] Write history entry.
- [x] Add release note.

## Decision Log

- 2026-05-23: start with tool-catalog sections before promotion state. Rationale:
  slice 69 already landed typed active-tool config, and spec 0017 needs a prompt
  builder contract before the fake-provider loop. Consequence: promotion remains a
  separate milestone, but the section/hashing contract becomes testable now.
- 2026-05-23: the first builder consumes `tool::CatalogRenderer` rather than moving
  schema rendering into `oran-prompt`. Rationale: schema canonicalisation and rendered
  block caching already live next to `tool::Registry`; duplicating it would create two
  prompt-facing tool formats. Consequence: `oran-prompt -> oran-tool` is a documented
  same-layer dependency.
- 2026-05-23: validation for milestone 1 passed:
  `xmake build oran-prompt`, `xmake build test-prompt`,
  `xmake build bench-prompt`, `xmake run test-prompt`,
  `xmake run bench-prompt`, `xmake build orangutan`,
  `xmake run orangutan --prompt smoke`, `xmake test`, and `make ci`.

## Linked Artifacts

- Related design doc: `docs/design-docs/agent-platform.md`
- Related product spec: `docs/product-specs/0016-prompt-and-tool-catalog-cache.md`
- PRs: TBD
- History entry:
  [`docs/histories/2026-05/20260524-0705-prompt-builder-skeleton.md`](../../histories/2026-05/20260524-0705-prompt-builder-skeleton.md)
- Release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)

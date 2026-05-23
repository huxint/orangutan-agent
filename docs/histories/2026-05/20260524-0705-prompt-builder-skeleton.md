# Prompt Builder Skeleton

The user asked to keep advancing the project one committed version at a time, with
docs synced to code. This slice opens the active prompt-builder v1 plan and lands its
first milestone: a new `oran-prompt` library with `prompt::Builder`,
`BuilderInputs`, `BuilderOptions`, `CacheSection`, `RenderedPrompt`, and
`SectionVersions`. The builder consumes `config::PromptActiveToolsConfig`, validates
explicit active-tool names against the provided catalog snapshot, delegates section
2/3 catalog bytes to `tool::CatalogRenderer`, emits the seven prompt-design sections,
and computes stable content hashes plus a cache-versioned prefix hash/byte count over
sections 1-6. The cache breakpoint is deliberately the per-agent overlay section, so
the conversation tail remains outside the cached prefix.

The design keeps schema rendering in `oran-tool` instead of duplicating it in
`oran-prompt`; that is why this slice documents and allowlists the intentional
`oran-prompt -> oran-tool` same-layer dependency. Promotion state, real skill/memory
renderers, provider cache mapping, and the agent-owned prompt-cache hit-rate fixture
remain in the active plan because they need `oran-agent` and session state. The new
tests cover default active selection, explicit active allowlists, explicit promotion
of a deferred tool, missing explicit names, stable prefixes across different
conversation tails, and cache-version invalidation. The new bench compares default
active-set assembly against an explicit two-tool active subset.

Files of interest:

- `include/oran/prompt/builder.hpp`
- `src/oran-prompt/builder.cpp`
- `tests/prompt/test_builder.cpp`
- `bench/prompt/scenarios/catalog_sections.cpp`
- `xmake/targets.lua`
- `scripts/check-deps.sh`
- `docs/exec-plans/completed/2026-05-23-prompt-builder-v1.md`
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md`
- `docs/design-docs/agent-platform.md`
- `docs/releases/feature-release-notes.md`

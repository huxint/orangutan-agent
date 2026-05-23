# 0016 — Prompt + Tool-Catalog Cache

## User Problem

Every model turn re-bills the system prompt unless the upstream cache
hits. Today the cache discipline lives in the rule
([`../rules/prompt-design.md`](../rules/prompt-design.md)) but no builder
exists; the section order, byte-identical-prefix invariant, and
`CacheSection` shape are all enforced by code that has not been written.
Once `oran-agent` lands and the catalog starts to grow, three failure
modes appear at the same time:

- **Catalog bloat in the prompt.** Six built-ins today is fine; the
  forward inventory (file/shell/memory/orchestration/automation/skill/
  MCP/search/background/attachments/runtime-loader/script per
  [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md)
  "Built-in Tool Categories") is ~50 tools. Sending every schema on
  every turn is wasted cache surface.
- **Drift in the cached prefix.** A single non-deterministic byte in
  any of sections 1–6 (clock, request id, randomized identity, per-call
  state) re-bills the full preamble. The rule is clear; without a
  builder and a regression bench, the next change adds the drift.
- **Cache that affects dispatch.** A "promoted deferred tool that
  expired from the catalog cache" must not become "tool the agent
  cannot call". The cache controls *prompt size*, never *tool
  availability*.

This spec defines the prompt + tool-catalog cache as a first-class
subsystem owned by `oran-prompt` (a library that does not yet exist),
consumed by `oran-agent` and the provider adapters. The rule provides
the *invariants*; this spec is the *what ships and how it is verified*.

## Scope (v1)

The MVP delivers the smallest builder that lets `oran-agent` send a
cacheable prompt with a deterministic tool catalog. Nothing else.

- **`oran-prompt` library skeleton**. New library; depends on
  `oran-core` (for `core::ToolDef`, `core::Message`, `core::Capability`),
  `oran-tool` (for `Registry::catalog()` snapshot), and `oran-memory`
  (forward-declared; populated when memory framing arrives).
- **`prompt::CacheSection`**:
  ```cpp
  struct CacheSection {
    std::string                   id;             // stable, human-readable
    std::string                   content;        // raw bytes
    std::uint64_t                 content_hash;   // xxh3 of content, computed at build
    std::uint32_t                 cache_version;  // bumped on content-rule change
    bool                          is_breakpoint;  // last section before tail when true
  };
  ```
  The exact field set is implementation-allowed to evolve; the
  load-bearing invariants are the rule's section order + the
  `(id, content_hash, cache_version)` tuple from
  [`../design-docs/api-portability.md`](../design-docs/api-portability.md)
  "Cache Key Versioning".
- **`prompt::Builder`**:
  ```cpp
  class Builder {
   public:
    struct Inputs {
      core::Identity                 identity;
      provider::ModelTarget          model;
      tool::CatalogSnapshot          tool_catalog;
      skill::CatalogSnapshot         skill_catalog;
      memory::Framing                memory_framing;
      std::vector<core::Message>     conversation_tail;
      std::optional<std::string>     per_agent_overlay;
    };

    Awaitable<core::Result<RenderedPrompt>>
    build(Inputs) const;
  };

  struct RenderedPrompt {
    std::vector<CacheSection> sections;  // ordered 1..7 per the rule
    std::uint64_t             prefix_hash; // hash of sections 1..6 concatenated
    std::size_t               prefix_bytes;
  };
  ```
  The agent loop calls `build(inputs)` once per turn. Adapter sees
  `RenderedPrompt` and maps to vendor cache shape per
  `api-portability.md`.
- **Deterministic tool-catalog renderer**:
  - **Status (slices 59 + 68):** the renderer exists in
    `oran-tool` as `tool::CatalogRenderer`, alongside the new
    `ToolDef::deferred` and `ToolDef::category` metadata. It renders
    active full-schema blocks and deferred name/description rows from a
    `Registry::catalog()` snapshot, sorts both by tool name, canonicalises
    JSON Schema bytes in `src/oran-tool/catalog.cpp`, and keeps a bounded
    256-entry rendered-block cache keyed by the fields that affect the
    block bytes plus renderer version, with aggregate stats. Slice 68 adds
    the registry-owned `tool.search` lookup primitive in `oran-tool`; the
    remaining bullets below that mention `oran-prompt`, active-tool config,
    and promotion state are still unimplemented.
  - Pure function of `ToolDef`:
    `(name, description, input_schema, required_capabilities,
    category)`.
  - Memoised by `(rendered ToolDef hash, renderer_version)`; the memo lives in a
    `BoundedCache<(hash, version), std::string>` (spec 0012) sized
    `runtime.prompt.tool_block_cache.max_entries` (default 256).
  - Output sorted by tool name to make the rendered bytes independent
    of registration order.
  - Capability list rendered via `core::enum_name` so reflection drives
    the bytes, not hand-written strings (rule already requires this).
- **Active vs. deferred catalog policy**:
  - Default active set:
    `file.read`, `file.write`, `file.edit` (or `file.modify` once spec
    0011 v2 lands), `file.search`, `directory.list`, `tool.search`.
  - Everything else registered with `ToolDef::deferred = true` lands
    in section 3 (deferred-tool index) as name + one-line description
    only.
  - The active set is config-driven:
    `runtime.prompt.active_tools` accepts an explicit allowlist or
    `"defaults"` to use the list above. Operators who add a new
    coding tool can promote it without touching code.
- **`tool.search` built-in** (the *non-deferred* lookup tool; future
  session promotion consumes its matches):
  - **Status (slice 68, 2026-05-24):** the registry lookup primitive is
    implemented in `oran-tool`; session promotion remains future agent
    work because `oran-agent::SessionState` does not exist yet.
  - Input: `{ name?, category?, capability? }`. At least one field
    required. Supplied fields are ANDed; `name` and `category` are exact
    string matches, and `capability` is parsed through
    `core::parse_enum<core::Capability>`.
  - Output: text fallback plus structured `Output::data_json` carrying
    `{kind:"tool_search", query, match_count, matches[]}`. Each match
    includes name, full description, nested full JSON schema,
    required capabilities, deferred flag, and nullable category.
  - Future side effect: append the matched tool name(s) to the
    per-session promotion set so the *next* turn's catalog includes the
    full schema. The promotion set lives on `oran-agent::SessionState`.
- **Promotion set bounding**:
  - LRU + TTL per the spec 0012 inventory
    (`max_entries=16`, `ttl=24h`).
  - Evicted promotions drop back to the deferred index; they are
    *never* removed from `Registry::catalog()`. Cache controls prompt
    size, not tool availability.
- **Cache breakpoint placement**. `RenderedPrompt::sections` always
  marks the boundary between section 6 (per-agent overlay) and section
  7 (conversation tail) as `is_breakpoint = true`. Adapters use this
  to place the vendor cache marker; no other section is a breakpoint
  in v1 (Anthropic allows 4; we ship 1 to keep the model simple).
- **Prompt-cache stability bench** —
  `bench/oran-agent/prompt_cache_hit_rate.cpp` (already named in the
  rule + tracked under the 2026-05-17 bench tech-debt row). N
  iterations against a recorded fixture; asserts `prefix_hash` is
  identical across iterations 2..N for every fixture in the suite.
  Failure means a drift sneaked in; CI fails.

## Scope (v1.1)

- **Multiple cache breakpoints** when measurement shows fan-out is
  worth it (Anthropic allows up to 4). Breakpoints land at section
  boundaries selected by the builder; the breakpoint set is
  deterministic per (model, route, agent).
- **Memory framing renderer** populates section 5. Pure function of
  `memory::Framing` per the rule.
- **Skill catalog renderer** populates section 4. Activating a skill
  shifts section 4, never section 1.
- **Active-set hot reload**. `runtime.prompt.active_tools` honours
  config reload without restart; the promotion set survives the
  reload; the cache invalidates by bumping `cache_version` for the
  affected section.
- **Per-route renderer variants**. Anthropic-only fields (`thinking`
  budget hints) ride in section 1's per-model overlay rather than
  forcing every adapter to share the same bytes.

## Scope (v2)

- **Cross-session prefix persistence** for routes that support
  customer-managed prompt caching (Anthropic with explicit cache
  keys, OpenAI Responses). The builder emits a stable cache key the
  adapter forwards verbatim.
- **Skill body activation** as a structured section update rather
  than a section-4 rewrite. Allows multiple skills active without
  every activation rebuilding the section.
- **Adaptive deferred-tool promotion**. The agent learns which
  deferred tools should be active for an identity based on past
  usage; promotion writes back to a per-identity preference store.

## Out Of Scope

- **A general-purpose templating engine.** The renderer is a small
  pile of pure functions, not a DSL. The rule's hard invariants make
  templating risky (a templating bug = a cache miss).
- **Caching content the *upstream* model provides** (assistant
  messages, tool results). Those are the conversation tail; they
  belong to section 7 by definition and are never cached.
- **Replacing the rule.** `docs/rules/prompt-design.md` stays the
  canonical home for cache-discipline invariants. This spec defers to
  it on every "what is allowed in section N" question.

## Acceptance Criteria

1. **Byte-identical prefix.** Two `Builder::build` calls with
   identical `Inputs` (same identity, same model, same catalog
   snapshot, same skill snapshot, same memory framing, same overlay,
   different conversation tails) produce identical `prefix_hash` and
   identical `sections[0..5]` content. Pinned by a regression test.
2. **Tool-block determinism.** Rendering the same `ToolDef` twice in
   the same process returns the *same byte sequence*. Pinned by a
   tool-renderer unit test that hashes the output and compares.
3. **Tool-block independence.** Rendering a `ToolDef` does not
   depend on `Registry::catalog()` order, on the current iteration
   counter, on the wall clock, or on `Identity` fields outside the
   stable set (`agent_name`, `team_id`). Compile-fail test asserts
   the renderer signature only accepts the stable inputs.
4. **Deferred-tool absence.** A tool registered with
   `deferred=true` does not appear in section 2's tool catalog;
   appears in section 3's deferred-tool index with name + one-line
   description only; does not appear in section 2 *even after the future
   `tool.search` promotion flow selects it* — promotion shifts the
   *next* turn's section 2, not the current.
5. **Future promotion semantics.** Calling `tool.search(name="memory.recall")`
   from a turn eventually adds `memory.recall` to the next turn's
   active catalog with its full schema. After 16 promotions in a
   session, the oldest evicts back to the deferred index (LRU per
   spec 0012).
   The evicted tool stays callable; the agent that uses it without
   re-promotion sees the same dispatch error as any other
   not-promoted call to a deferred tool (`tool.search` is the
   reminder, not a precondition).
6. **Cache version bump invalidates.** Bumping `cache_version` on
   any section produces a different `prefix_hash` even when content
   is byte-identical. Used to invalidate without changing content.
7. **Cache controls bytes, not availability.** A tool that is in
   `Registry::catalog()` but absent from the rendered prompt
   (deferred, not promoted, or evicted from promotion) is still
   callable through `Registry::dispatch` if the agent emits a
   `tool_use` for it. The dispatch records `AuditEvent.context.
   was_in_prompt=false` for observability.
8. **Breakpoint placement.** The adapter sees exactly one
   `is_breakpoint = true` section in v1, at the boundary between
   section 6 and section 7. The adapter test asserts the vendor
   cache marker lands at the right byte offset.
9. **Active-set config drives section 2.** A config with
   `runtime.prompt.active_tools = ["file.read", "file.search"]`
   produces a section 2 that contains *only* those two tools; every
   other registered tool moves to section 3.
10. **Prompt-cache stability bench.** Running
    `bench/oran-agent/prompt_cache_hit_rate.cpp` against three
    fixtures (small catalog, large catalog, with-promotion) produces
    `prefix_hash` identical across iterations 2..N for every
    fixture. CI fails on any drift.
11. **`tests/prompt/`** ≥ 90% coverage of the matrix
    (section × deterministic-input × cache_version × promotion
    state × breakpoint placement × config override).

## Design Doc Cross-References

- [`../rules/prompt-design.md`](../rules/prompt-design.md) —
  **canonical home for the invariants.** Section order, hard
  invariants ("no clocks", "no request IDs", etc.), and minimum
  cacheable block size all live there. This spec defers; it never
  restates.
- [`../design-docs/api-portability.md`](../design-docs/api-portability.md)
  — `CacheSection` shape, cache-key versioning, per-adapter mapping
  (Anthropic `cache_control` vs. OpenAI Responses prefix hashing).
  Adapters consume `RenderedPrompt`, the spec defines what's in it.
- [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md)
  "Deferred Tools" — the per-agent promoted set sketch is the v1
  shape this spec ships.
- [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)
  "Prompt Assembly (deferred)" — the placeholder for prompt-builder
  decisions. The first builder slice updates it with the adopted
  Piebald-AI shapes per the rule's "Adding A New Prompt Surface"
  checklist.
- [`0017-fake-provider-first-agent-loop.md`](0017-fake-provider-first-agent-loop.md)
  — `prompt::Builder` is the loop's prompt source from v1 onward;
  the fake provider pins prompt/response behaviour before real
  adapters exist.
- [`0018-first-loop-observability.md`](0018-first-loop-observability.md)
  — records `RenderedPrompt::prefix_hash`, prefix bytes,
  active/deferred catalog hashes, and cache-token usage per turn.
- [`0014-structured-tool-output.md`](0014-structured-tool-output.md)
  — `tool.search` uses the shipped `Output::data_json` structured
  channel plus a text fallback.
- [`0012-tool-scheduler-and-state.md`](0012-tool-scheduler-and-state.md)
  — the rendered-block cache and the promotion-set bound both use
  the `BoundedCache` primitive defined there.
- [`0009-skills.md`](0009-skills.md) — section 4 (skills catalog)
  consumes the skill loader's catalog snapshot.
- [`0010-benchmark-harness.md`](0010-benchmark-harness.md) — the
  prompt-cache-hit-rate fixture shape lives here once the bench
  scenario is authored.

## Risks

- **Latent drift before the bench lands.** Slices that ship sections
  1–6 without the cache-stability bench can introduce drift that
  goes undetected. Mitigation: v1 acceptance criterion #10 is
  blocking — the bench ships with v1, not as a follow-up.
- **Promotion set fights cache stability.** If promotion mutates
  section 2 mid-session, every promotion is a cache miss for the
  whole prefix. Mitigation: promotion only shifts the *next* turn's
  section; within a turn, section 2 is frozen at `build` time. The
  cache miss is then paid once per promotion, not per turn.
- **Active-set config explosion.** Operators may want fine-grained
  active sets per agent. Mitigation: v1 ships a global active set;
  v1.1 layers per-agent overrides via the existing
  `config.agents.<name>.permissions`-shaped overlay. Until then,
  one active set per process.
- **JSON schema renderer pretty-printing.** Pretty-printed JSON is
  prone to whitespace drift. Mitigation: schemas render through the
  same serializer with a single fixed `dump(indent=2,
  ensure_ascii=false)` call; the renderer's bytes are unit-tested.

## Validation

```sh
xmake build oran-prompt
xmake test test-prompt                    # builder determinism + promotion + breakpoint
xmake build bench-oran-agent
xmake run bench-oran-agent prompt_cache_hit_rate
xmake run orangutan -- --explain-prompt   # planned debug surface; lands with builder
```

## Out-of-Band Cross-Cuts

- `docs/ARCHITECTURE.md` "Library Inventory" — the `oran-prompt` row
  flips from "planned" to "skeleton" in the slice that lands v1.
- `docs/design-docs/agent-platform.md` "Prompt Assembly (deferred)"
  — the placeholder is replaced by a "Prompt Assembly" section
  recording which Piebald-AI shapes were adopted, with one-line
  rationale per choice.
- `docs/rules/prompt-design.md` "Enforcement" — `scripts/
  check-prompt-preamble` static grep tracked under the 2026-05-17
  prompt tech-debt row lands in the same slice as v1, since the
  first stable preamble template now exists.
- `docs/exec-plans/tech-debt-tracker.md` — the 2026-05-17
  `bench/oran-agent/prompt_cache_hit_rate.cpp` row closes when v1
  ships; the `check-prompt-preamble` row closes in the same arc.
- `docs/STATUS.md` — `oran-prompt` reaches `C` (per
  [`QUALITY_SCORE.md`](../QUALITY_SCORE.md)) when v1 lands.

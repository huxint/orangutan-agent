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
subsystem owned by `oran-prompt` and consumed by `oran-agent` plus the
provider adapters. The rule provides
the *invariants*; this spec is the *what ships and how it is verified*.

## Scope (v1)

The MVP delivers the smallest builder that lets `oran-agent` send a
cacheable prompt with a deterministic tool catalog, plus the session-owned
promotion side effect that the first agent loop will reuse.

- **`oran-prompt` library skeleton**.
  - **Status (slice 71, 2026-05-24):** the library exists with
    `prompt::Builder`, `BuilderInputs`, `BuilderOptions`, `CacheSection`,
    `RenderedPrompt`, `SectionVersions`, `prompt::PromotionState`,
    `test-prompt`, and `bench-prompt`. It depends on `oran-core` (messages /
    tool definitions / explicit `Time` inputs), `oran-async` (awaitable
    contract), `oran-config` (active-tool selector), and `oran-tool` (catalog
    bytes via `CatalogRenderer`). Memory and skill section inputs now have
    their own owners outside `oran-prompt`; `BuilderInputs` still accepts the
    pre-rendered strings.
- **`prompt::CacheSection`**:
  ```cpp
  struct CacheSection {
    std::string                   id;             // stable, human-readable
    std::string                   content;        // raw bytes
    std::uint64_t                 content_hash;   // stable hash of content, computed at build
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
  struct BuilderInputs {
    std::string_view                 system_preamble;
    std::span<const core::ToolDef>   tool_catalog;
    config::PromptActiveToolsConfig  active_tools;
    std::span<const std::string>     promoted_tools;
    std::string_view                 skills_catalog;
    std::string_view                 memory_framing;
    std::string_view                 per_agent_overlay;
    std::span<const core::Message>   conversation_tail;
  };

  class Builder {
   public:
    Awaitable<core::Result<RenderedPrompt>> build(BuilderInputs);
  };

  struct RenderedPrompt {
    std::vector<CacheSection> sections;  // ordered 1..7 per the rule
    std::uint64_t             prefix_hash; // cache-versioned hash of sections 1..6
    std::size_t               prefix_bytes;
  };
  ```
  `promoted_tools` is the sorted snapshot from `prompt::PromotionState`.
  Missing snapshot names are ignored by catalog selection because they are
  stale session hints; explicit config allowlists still validate against the
  catalog snapshot and return `ErrorKind::not_found` on missing names.
  The agent loop calls `build(inputs)` once per turn. Adapter sees
  `RenderedPrompt` and maps to vendor cache shape per
  `api-portability.md`.
  **Status (slice 73, 2026-05-24):** `oran-provider` now exposes
  `provider::make_prompt_cache_hints(RenderedPrompt, options)`, the first
  adapter-facing mapper for that contract. It validates the seven-section
  prompt shape, the single section-6 breakpoint, and the prefix-byte count,
  then returns section 1-6 `(id, content_hash, cache_version)` keys plus the
  prefix hash/bytes while excluding section 7's conversation tail.
- **Deterministic tool-catalog renderer**:
  - **Status (slices 59 + 68 + 69 + 70 + 71):** the renderer exists in
    `oran-tool` as `tool::CatalogRenderer`, alongside the new
    `ToolDef::deferred` and `ToolDef::category` metadata. It renders
    active full-schema blocks and deferred name/description rows from a
    `Registry::catalog()` snapshot, sorts both by tool name, canonicalises
    JSON Schema bytes in `src/oran-tool/catalog.cpp`, and keeps a bounded
    256-entry rendered-block cache keyed by the fields that affect the
    block bytes plus renderer version, with aggregate stats. Slice 68 adds
    the registry-owned `tool.search` lookup primitive in `oran-tool`.
    Slice 69 adds the typed `runtime.prompt.active_tools` config surface in
    `oran-config`; slice 70 consumes that selector in `prompt::Builder`.
    Slice 71 adds `prompt::PromotionState` plus builder consumption of its
    sorted snapshot. Slice 72 adds `agent::SessionState` as the first owner
    that observes successful `tool.search` outputs and mutates that state.
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
  - Deferred tools not selected by active config or the promotion snapshot
    land in section 3 (deferred-tool index) as name + one-line description
    only.
  - The active set is config-driven:
    `runtime.prompt.active_tools` accepts an explicit allowlist or
    `"defaults"` to use the list above. Operators who add a new
    coding tool can promote it without touching code.
  - **Status (slice 71, 2026-05-24):** `oran-config` parses this field into
    `config::RuntimeConfig::prompt.active_tools`. The `"defaults"`
    sentinel sets `use_defaults=true`; an array preserves the authored
    tool-name allowlist with `use_defaults=false`; empty explicit
    allowlists are valid. The loader rejects malformed shapes and empty
    tool names but does not resolve names against `tool::Registry`, because
    config is below the tool layer. `prompt::Builder` resolves explicit
    names against the tool catalog snapshot and returns `ErrorKind::not_found`
    for missing names.
- **`tool.search` built-in** (the *non-deferred* lookup tool; session
  promotion consumes its matches):
  - **Status (slice 72, 2026-05-24):** the registry lookup primitive is
    implemented in `oran-tool`; `prompt::PromotionState` exists in
    `oran-prompt`; and `agent::SessionState` now observes successful
    `tool.search` `Output::data_json`, validates
    `{kind:"tool_search", matches[]}`, and promotes only deferred matches by
    name into the next prompt snapshot. Non-search outputs and failed
    `tool.search` outputs are no-ops; malformed successful structured data
    returns `ErrorKind::invalid_argument` without mutating state.
  - Input: `{ name?, category?, capability? }`. At least one field
    required. Supplied fields are ANDed; `name` and `category` are exact
    string matches, and `capability` is parsed through
    `core::parse_enum<core::Capability>`.
  - Output: text fallback plus structured `Output::data_json` carrying
    `{kind:"tool_search", query, match_count, matches[]}`. Each match
    includes name, full description, nested full JSON schema,
    required capabilities, deferred flag, and nullable category.
  - Side effect: `agent::SessionState` appends deferred matched tool name(s)
    to its per-session `prompt::PromotionState` so the *next* turn's catalog
    includes the full schema.
- **Promotion set bounding**:
  - **Status (slice 71, 2026-05-24):** `prompt::PromotionState` ships this
    bounded state with LRU + TTL per the spec 0012 inventory
    (`max_entries=16`, `ttl=24h`). It takes explicit `core::Time` values so
    prompt sections never read hidden clocks, reports aggregate stats, rejects
    empty tool names, and returns sorted snapshots for deterministic section-2
    bytes.
  - Evicted promotions drop back to the deferred index; they are
    *never* removed from `Registry::catalog()`. Cache controls prompt
    size, not tool availability.
- **Cache breakpoint placement**. `RenderedPrompt::sections` always
  marks the boundary between section 6 (per-agent overlay) and section
  7 (conversation tail) as `is_breakpoint = true`. Adapters use this
  to place the vendor cache marker; no other section is a breakpoint
  in v1 (Anthropic allows 4; we ship 1 to keep the model simple).
  Slice 73 pins that adapter-side boundary with
  `provider::make_prompt_cache_hints`, which rejects missing, duplicate,
  misplaced, or prefix-byte-drifted breakpoints and supports disabled /
  minimum-prefix skip paths for routes that cannot use upstream caching.
- **Prompt-cache stability bench** —
  `bench/agent/scenarios/prompt_cache_hit_rate.cpp`. N iterations against a
  session-owned fixture; asserts `prefix_hash` is identical across changing
  conversation tails after the promotion snapshot is fixed. Slice 72 ships
  the no-promotion and after-promotion paths (`agent.prompt_cache_no_promotions`
  about 54.4 us / fixture; `agent.prompt_cache_after_promotion` about 63.1 us /
  fixture). Failure means a drift sneaked in; CI fails once the bench gate is
  wired.

## Scope (v1.1)

- **Multiple cache breakpoints** when measurement shows fan-out is
  worth it (Anthropic allows up to 4). Breakpoints land at section
  boundaries selected by the builder; the breakpoint set is
  deterministic per (model, route, agent).
- **System preamble owner** populates section 1.
  **Status (slice 134, 2026-06-01):** `oran-agent` now exports
  `agent::SystemPreamble`, `agent::SystemPreambleOwner`, and
  `agent::default_system_preamble()`. `agent::Loop` uses its owned default
  when callers leave `RunTurnInputs::system_preamble` empty, while
  `AgentPromptRunner` renders the default once before loop entry for the
  configured-route path. The default text is stable section-1 runtime contract
  only; `scripts/check-prompt-preamble.sh` rejects clocks, ids, and
  cross-section prompt bytes.
- **Memory framing renderer** populates section 5.
  **Status (slice 164, 2026-06-05):** `oran-memory` now exports
  `memory::Framing` / `memory::FramingOwner`, a minimal once-per-turn owner
  for already-materialized section-5 bytes. `AgentPromptRunner` renders it
  before entering `agent::Loop`, and `prompt::Builder` continues to consume the
  stable `BuilderInputs::memory_framing` string. Slice 164 adds opt-in
  long-term recall in `AgentPromptRunnerOptions::longterm_recall`: the runner
  fills `memory::Framing` from `memory::longterm::Runtime::recall(...)` once at
  the prompt boundary without changing the builder or re-querying inside
  provider/tool iterations. Ordinary configured-route startup does not enable
  that option until config/query policy lands.
- **Skill catalog renderer** populates section 4.
  **Status (slice 138, 2026-06-01):** `oran-skill` now ships the
  deterministic section-4 catalog renderer, the section-4 owner, and the first
  markdown loader snapshot. `AgentPromptRunner` loads the workspace skills
  directory before the first prompt when configured, then refreshes the
  workspace skill snapshot at later prompt boundaries when watcher/signature
  changes are visible; `prompt::Builder` still consumes the stable
  `BuilderInputs::skills_catalog` string for the current turn. Skill bodies
  remain outside the system preamble and outside the catalog. `skill.invoke`
  returns the matched loaded snapshot body through the ordinary conversation-tail
  tool-result path, so it does not change the cached prefix. Add/update/remove
  skill changes intentionally break section-4 cache bytes on the next prompt,
  not in the middle of an active turn. Activating a future persistent skill
  section shifts section 4, never section 1.
  **Status (doc slice + slices 144-145, 2026-06-02):** section-4 activation
  policy cache semantics are explicit, and `skill::ActivationPolicy` now has
  explicit deactivation and expiration inputs. The skills section is a pure render of
  the loaded/allowed skill metadata snapshot plus explicit policy inputs and
  transcript/durable activation state. Given identical inputs, repeated renders
  must be byte-identical. A changed active marker, deactivation event, or expiry
  is an intentional section-4 content change for the next prompt and therefore
  invalidates the cached prefix through the section content hash. A change to
  the marker interpretation or rendering rule requires a
  `SectionVersions::skills_catalog` bump. Skill bodies remain conversation-tail
  tool results and exact caller-supplied `skills_catalog` bytes bypass
  automatic snapshot/policy handling.
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
   description only; does not appear in section 2 *during the same turn
   where `tool.search` discovers it* — promotion shifts the
   *next* turn's section 2, not the current.
5. **Promotion semantics.** Calling `tool.search(name="memory.recall")`
   from a turn lets `agent::SessionState` add `memory.recall` to the next turn's
   active catalog with its full schema. After 16 promotions in a
   session, the oldest evicts back to the deferred index (LRU per
   spec 0012). Slice 71 ships the state and builder snapshot consumption;
   slice 72 ships the agent-owned `tool.search` side effect.
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
   cache marker lands at the right byte offset. Slice 73 ships the
   provider-side validation/mapping half in `test-provider`: successful
   mappings expose only sections 1-6, disabled and size-floor routes skip
   cache hints cleanly, and malformed boundaries return
   `ErrorKind::invalid_argument`.
9. **Active-set config drives section 2.** A config with
   `runtime.prompt.active_tools = ["file.read", "file.search"]`
   produces a section 2 that contains *only* those two tools; every
   other registered tool moves to section 3. Parser status: slice 69
   validates both config shapes and exposes the typed data; builder status:
   slice 70 renders the explicit active set and validates missing names in
   `test-prompt`.
10. **Prompt-cache stability bench.** Running
    `bench-agent`'s `prompt_cache_hit_rate` fixture produces
    `prefix_hash` identical across changing conversation tails for the
    no-promotion and after-promotion session snapshots. CI fails on any drift
    once the bench gate is wired. Slice 72 ships the agent-owned fixture;
    larger recorded fixtures can extend the same scenario when the full loop
    lands.
11. **Skill activation cache boundary.** Activation, deactivation, and expiry
    policy changes section 4 only at prompt boundaries. Identical loaded/allowed
    skill snapshots plus identical policy inputs produce identical section-4
    bytes; changed active-marker state changes section 4 and the prefix hash for
    the next prompt only; invoked skill bodies still enter only through section
    7 as tool-result text.
12. **`tests/prompt/`** ≥ 90% coverage of the matrix
    (section × deterministic-input × cache_version × promotion
    state × breakpoint placement × config override). Slice 71 covers
    section order, breakpoint count, cache-version invalidation,
    tail-independent prefix hashes, default/explicit config overrides,
    explicit promotion of a deferred tool, builder consumption of a promotion
    snapshot, and promotion-state LRU / TTL / validation semantics.

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
  — the rendered-block cache uses `BoundedCache`; the promotion set follows
  the same bounded-state policy but ships as a dedicated enumerable
  `prompt::PromotionState` because prompt assembly needs sorted live names and
  promotion-vs-refresh stats.
- [`0009-skills.md`](0009-skills.md) — section 4 (skills catalog)
  consumes the skill loader/snapshot owner; `skill.invoke` returns body text as
  a tool result, and watcher/signature refresh updates section 4 before the next
  prompt.
- [`0010-benchmark-harness.md`](0010-benchmark-harness.md) — the
  prompt-cache-hit-rate fixture shape lives here once the bench
  scenario is authored.

## Risks

- **Latent drift before the full loop lands.** Slices that ship sections
  1–6 without recorded loop fixtures can introduce drift outside the
  current synthetic session coverage. Mitigation: `bench-prompt` exercises
  deterministic builder inputs, and slice 72's `bench-agent` fixture now pins
  the session-owned no-promotion and after-promotion cache paths.
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
xmake run test-prompt                     # builder determinism + promotion state + breakpoint
xmake run test-agent                      # SessionState tool.search promotion owner
xmake run test-config                     # active-tool config parser contract
xmake run bench-prompt                    # prompt-owned precursor bench, including promoted snapshots
xmake run bench-agent                     # agent-owned prompt-cache stability fixture
xmake run orangutan -- --explain-prompt   # planned debug surface; lands with agent/prompt wiring
```

## Out-of-Band Cross-Cuts

- `docs/ARCHITECTURE.md` "Library Inventory" — the `oran-prompt` row
  flips from "planned" to "skeleton" in the slice that lands v1.
- `docs/design-docs/agent-platform.md` "Prompt Assembly (deferred)"
  — the placeholder is replaced by a "Prompt Assembly" section
  recording which Piebald-AI shapes were adopted, with one-line
  rationale per choice.
- `docs/rules/prompt-design.md` "Enforcement" — slice 134 adds
  `scripts/check-prompt-preamble.sh` and wires it into `scripts/ci.sh` for the
  default `oran-agent` section-1 preamble.
- `docs/exec-plans/tech-debt-tracker.md` — the 2026-05-17
  prompt-cache bench row closes in slice 72 because `bench-agent` now owns
  the SessionState fixture; `bench-prompt` remains the prompt-owned precursor.
- `docs/STATUS.md` — `oran-prompt` reaches `C` (per
  [`QUALITY_SCORE.md`](../QUALITY_SCORE.md)) when v1 lands.

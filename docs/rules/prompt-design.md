# Prompt Design

> **Why this rule exists.** The thing this project builds — an LLM agent
> runtime — *is* a prompt-emitter. Every system prompt, tool description,
> skill body, memory framing, and channel adapter contributes bytes to a
> request that an upstream model bills, caches, and reasons over. Prompt
> design is a load-bearing engineering surface, not creative writing.
>
> **Why this rule is short.** Prompt engineering evolves with the
> features it serves. This rule captures the invariants that hold today
> (cache discipline) and points at a study reference for the patterns we
> have not yet had to design. Rows are added as Orangutan-owned
> prompt-bearing libraries land (agent loop → tool registry render →
> skill loader → memory framing → channel framing).

## Runtime Prompt Surfaces

This table routes prompt surfaces that the Orangutan runtime will emit
to an upstream model. It is not the tutorial for the development-agent
prompt framework (`CLAUDE.md` / `AGENTS.md`); it only governs prompt
bytes produced by this project.

| Surface | Canonical file(s) | Owns |
| --- | --- | --- |
| Invariants / cache section membership | This rule | Section order, cacheability rules, "no clocks / request IDs in the cached prefix", stable-input renderer discipline. |
| Prompt builder + rendered prompt contract | [`../product-specs/0016-prompt-and-tool-catalog-cache.md`](../product-specs/0016-prompt-and-tool-catalog-cache.md) | `oran-prompt`, `prompt::Builder`, `CacheSection`, prefix hash, active/deferred catalog, `tool.search`, prompt-cache stability bench. |
| Provider cache mapping | [`../design-docs/api-portability.md`](../design-docs/api-portability.md) | Adapter mapping from `RenderedPrompt` to vendor cache APIs and cache-key versioning. |
| Tool catalog / deferred tools | [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md), [`../product-specs/0012-tool-scheduler-and-state.md`](../product-specs/0012-tool-scheduler-and-state.md) | Tool catalog shape, deferred-tool promotion state, bounded caches used by renderers and schedulers. |
| Structured tool results in prompts | [`../product-specs/0014-structured-tool-output.md`](../product-specs/0014-structured-tool-output.md) | `ToolOutput { text, data, attachments, usage, is_error }`, byte caps, hook-safe redaction fields. |
| Approval / blocking prompt text | [`../product-specs/0015-blocking-hook-decisions.md`](../product-specs/0015-blocking-hook-decisions.md), [`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md) | `permission_ask_rendered`, blocking decisions, canonical dispatch order before tool mutation. |
| Skill catalog / skill bodies | [`../product-specs/0009-skills.md`](../product-specs/0009-skills.md) | Section-4 skill catalog and per-skill body constraints. |
| Agent-loop consumer | [`../product-specs/0017-fake-provider-first-agent-loop.md`](../product-specs/0017-fake-provider-first-agent-loop.md), [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md) | The first loop calls `prompt::Builder` against a fake provider before any real adapter. |
| Prompt observability | [`../product-specs/0018-first-loop-observability.md`](../product-specs/0018-first-loop-observability.md) | Per-turn prompt prefix hash, prefix byte count, active/deferred catalog hashes, cache usage counters. |

## The Study Reference

When a slice introduces a new prompt surface — a system prompt builder,
a tool catalog renderer, a skill body template, a memory framing block,
a deferred-tool index, an approval-prompt template, anything an upstream
model reads — **consult**:

- **<https://github.com/Piebald-AI/claude-code-system-prompts>** — distilled
  Claude Code system-prompt corpus. Use it as the *catalog of proven
  shapes* before designing a new one from scratch:
  - How a tool catalog is laid out (one block per tool, name + one-line
    description + JSON Schema, no per-invocation jitter).
  - Where invariant context (workspace root, capabilities, identity)
    lives vs. where iteration-scoped context (last assistant turn,
    pending tool result) lives.
  - How "deferred tools" / search-on-demand patterns compress the
    catalog.
  - Sentinel structure for hidden instructions vs. agent-visible text.

Record the patterns you adopted (or rejected with a reason) in the
matching design doc — `docs/design-docs/agent-platform.md` for the
agent loop, `docs/design-docs/tool-runtime.md` for tool rendering,
`docs/product-specs/0009-skills.md` for skill body shape, etc. The
reference URL is *external*; the decisions are *internal* and live in
the repo.

If the reference URL ever moves or 404s, **fix this file in the same
PR** that discovers the breakage. Reference rot is a Prime Directive
violation.

## Cache Discipline — The Hard Rule

Anthropic prompt caching reuses a *prefix* across requests. A single
byte of drift in the prefix breaks the cache hit and re-bills the full
preamble. The same posture applies to OpenAI Responses' prefix hashing.
The project must produce **byte-identical preambles across iterations
when the underlying inputs are unchanged** — otherwise the agent's
recurring per-iteration cost is the full system prompt every time, not
just the new conversation tail.

The agent's prompt builder produces a list of `CacheSection`s; the
provider adapter maps each to the vendor cache API (`cache_control` for
Anthropic, prefix hashing for OpenAI Responses). See
[`docs/design-docs/api-portability.md` "Caching"](../design-docs/api-portability.md).

### Section Order (Stable → Dynamic)

The prompt is assembled in this order, oldest-stable to newest-dynamic:

1. **System preamble** — agent identity, operating principles, output
   shape contract, error model. Changes only on slice boundaries.
2. **Tool catalog** — one entry per active registered tool, where the
   active set is selected by `runtime.prompt.active_tools` (`"defaults"`
   or an explicit allowlist) plus the session's promoted-tool snapshot.
   Promoted deferred tools move into this full-schema catalog on the next
   build. The available tool universe changes when `tool::Registry::add` /
   `remove` runs; *the rendering of a single tool's description must be
   deterministic in the tool's static fields*, never the call site.
3. **Deferred-tool index** — remaining deferred tools as name + one-line
   description only (no schema). The shipped `tool.search` returns full
   metadata on demand; session promotion moves selected tools into the next
   turn's full-schema catalog.
4. **Skills catalog** — compact listing of activated skills. Adding /
   removing a skill is a cache-break by design. Activation, deactivation, and
   expiration policy also lives here: policy inputs are resolved before prompt
   rendering, active markers are deterministic metadata-only rows, and any
   changed marker set intentionally changes section-4 bytes for the next
   prompt. Skill bodies still stay out of sections (1)-(6).
5. **Memory framing** — working-memory + session-memory summary. The
   summary text must be a function of memory state alone; do *not*
   thread "current time" or "request id" through it.
6. **Per-agent overlay** — agent-specific instructions from
   `config.agents.<name>.*`. Stable within an agent's run.
7. **Conversation tail** — past turns + the new user turn. This is the
   only intentionally dynamic block; the cache boundary sits here.

Adapters must place the cache breakpoint at the boundary between (6)
and (7). Sections (1)–(6) are the cached prefix. The conversation tail
is *not* cached.

### Hard Invariants

These are review-blocking violations.

- **No clocks in the preamble.** No `now()`, no `format_iso8601_utc(...)`,
  no "today is Tuesday" interpolation in sections (1)–(6). If the agent
  needs the wall clock, it asks a tool — that's section (7) territory.
- **No request IDs, trace IDs, or per-call counters in the preamble.**
- **No `std::format` of a randomized `Identity`** in the preamble. The
  parts of `Identity` that *are* stable for an agent's lifetime
  (`agent_name`, `team_id`) can appear; per-invocation parts cannot.
- **Tool descriptions are pure functions of `ToolDef`.** A tool's
  rendered block must depend only on its `name`, `description`,
  `input_schema`, `requires`, and `category` — never on the calling
  agent's recent activity, the current iteration count, or the elapsed
  time. The `ToolDef` is captured at `Registry::add` and its rendered
  form is memoized.
- **Skill bodies live in their own section, not inline in the system
  preamble.** Activating a skill must shift section (4), not rewrite
  section (1).
- **Skill activation policy is prompt-boundary state.** Activation,
  deactivation, and expiration rules must produce the same section-4 bytes for
  the same loaded/allowed skill snapshot, transcript/policy state, and explicit
  policy inputs. A policy result may change section (4) only before the next
  prompt. If the rule for rendering or interpreting markers changes, bump the
  section's cache-version; if only the active marker set changes, the content
  hash and prefix hash provide the cache break.
- **No "you have already done X" status text** glued into the preamble.
  That's conversation history; it lives in section (7).
- **Minimum cacheable block size.** Anthropic caches blocks of ≥ 1024
  tokens (Sonnet/Opus tier) or ≥ 2048 tokens (Haiku tier). Sections
  smaller than the floor should be merged with an adjacent stable
  section — do not register a 200-token "preamble" + 200-token "tool
  catalog" as separate cache breakpoints; merge them.
- **Cache-key versioning is explicit.** Each `CacheSection` carries a
  `cache_version` integer; bump the version when a stable section's
  *content rules* change (a new tool shape, a reworded preamble), not
  on every build. See `api-portability.md` "Cache Key Versioning".

### Soft Guidelines

- Prefer **one large stable section over many small ones**. Each
  breakpoint costs a cache-control entry on Anthropic (max 4) and a
  cache-miss risk surface.
- When rendering anything user-visible into the preamble (workspace
  path, model name), normalize it once at agent construction and store
  the normalized string; do not re-normalize per request.
- Prefer enum identifiers (`Capability::read_file`,
  `Mode::default_`) rendered via `core::enum_name` over hand-written
  strings — the renderer is then deterministic by reflection.

## Enforcement

Two layers:

- **Code review.** Anything that adds bytes to sections (1)–(6) is
  reviewed for membership in this rule. Reviewers flag clocks, per-call
  IDs, conversation status, and inline skill bodies.
- **Bench / regression gate.** `bench-agent` ships the SessionState-owned
  `prompt_cache_hit_rate` scenario that runs synthetic prompt builds before
  and after a `tool.search` promotion and aborts if the cached-prefix
  `RenderedPrompt::prefix_hash` drifts across changing conversation tails.
  Future full-loop fixtures can extend the same bucket with recorded turns.

- **Static grep.** `scripts/check-prompt-preamble.sh` runs in `scripts/ci.sh`
  and scans the default `oran-agent` section-1 preamble for clocks, ids,
  randomness, and bytes owned by tool, skill, memory, or conversation sections.
  The grep is intentionally narrow; it protects the versioned default preamble
  and does not replace code review for future renderer inputs.

## Adding A New Prompt Surface

When a slice introduces a new prompt-bearing artifact:

1. Read this rule.
2. Read the relevant section of
   <https://github.com/Piebald-AI/claude-code-system-prompts> for prior
   art. If you adopt a shape, record which one. If you reject it,
   record why.
3. Decide which `CacheSection` your text belongs to. If none fits, the
   rule needs a new row — propose the row in the same PR.
4. Implement the renderer as a pure function of its stable inputs.
5. Add a test that proves byte-identical output for two calls with
   identical inputs.
6. Add a bench scenario (or extend the existing one) covering the new
   section.

## See Also

- [`../design-docs/api-portability.md` "Caching"](../design-docs/api-portability.md)
  — `CacheSection`, cache-key versioning, adapter mapping.
- [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md)
  — tool catalog and deferred-tool surface.
- [`../product-specs/0016-prompt-and-tool-catalog-cache.md`](../product-specs/0016-prompt-and-tool-catalog-cache.md)
  — builder contract, active/deferred catalog policy, cache stability bench.
- [`../product-specs/0017-fake-provider-first-agent-loop.md`](../product-specs/0017-fake-provider-first-agent-loop.md)
  — first consumer of `prompt::Builder`.
- [`../product-specs/0018-first-loop-observability.md`](../product-specs/0018-first-loop-observability.md)
  — trace fields that prove which prompt prefix and catalog were used.
- [`../product-specs/0009-skills.md`](../product-specs/0009-skills.md)
  — skill body shape and per-skill body size cap.

Slices 135-146 land the first deterministic section-4 owner, loader snapshot,
one-shot `skill.invoke` dispatch path, prompt-boundary hot-reload,
runner-selected per-agent filtering, and transcript-derived active skill
markers plus an explicit `skill::ActivationPolicy` resolver with
caller-supplied deactivation names and expiration inputs in `oran-skill`,
`oran-tool`, and bootstrap, now sourced from per-agent
`agents.<name>.skills_deactivated` / `skills_expirations` config with a
runner-supplied prompt-boundary evaluation time.
That owner keeps skill bodies out of section 1 and renders a compact
metadata-only catalog before loop entry; `skill.invoke` returns body text as an
ordinary conversation-tail tool result, so it does not mutate cached sections
(1)-(6) during an active turn. Watcher / signature refresh, per-agent allowlist
filtering, successful activation metadata, explicit deactivation policy inputs,
and explicit expiration policy inputs replace section-4 bytes before the next
prompt when skill files, selected agent config, loaded transcript state, or
policy state change.
- [`../product-specs/0010-benchmark-harness.md`](../product-specs/0010-benchmark-harness.md)
  — where the cache-hit-rate bench will live.

# 0009 — Skills

## User Problem

Some prompts are reusable: "review a PR", "summarize a doc", "write a release note".
Encoding these as **skills** — short markdown templates with metadata — lets the agent
activate them on demand without bloating the system prompt.

## Scope (v1)

- `oran-skill::Loader` reading skills from `<workspace>/.orangutan/skills/<name>.md`.
  **Status (slices 136-139, 2026-06-01):** the loader snapshots existing
  `<workspace>/.orangutan/skills/*.md` files through `oran-io`, parses
  single-line YAML-style frontmatter metadata, enforces a 4 KiB default body
  cap plus a separate frontmatter cap, treats a missing skills directory as an
  empty snapshot, and can render the resulting compact catalog. Bootstrap
  configured-route prompts now keep a `skill::WorkspaceSkillSnapshot` for that
  directory unless the caller supplied exact `skills_catalog` bytes. The
  snapshot loads before the first prompt, drains Linux inotify events when
  available, compares a bounded content-aware `*.md` directory signature before
  each later prompt, invalidates matching `oran-io` file-view cache entries
  before re-reading changed skill files, and replaces both the compact catalog
  and the invocation document snapshot together. A turn therefore sees one
  coherent catalog/body snapshot; file changes are visible to the next prompt
  without restart.
- Skill metadata in YAML frontmatter:
  - `name`, `description`, `triggers` (semantic intents), `inputs` (schema), `model_hint`.
- Skill catalog rendered into the system prompt (compact listing).
  Catalog rendering and skill-body placement follow the cache-section
  ordering in [`docs/rules/prompt-design.md`](../rules/prompt-design.md) —
  activated skill bodies shift the skills section, never the system
  preamble. **Status (slices 142-149, 2026-06-04):** `oran-skill` can render
  deterministic `Active Skill: <name>` markers ahead of the ordinary compact
  skill entries. The current `skill::ActivationPolicy` derives markers from
  versioned `skill.invoke` metadata in the session transcript and filters them
  through the current loaded/allowed catalog entries before section 4 is
  rendered for the next prompt. Slice 148 adds durable
  `session_skill_activations` rows as an overlay on that transcript-derived set,
  so session-memory callers can preserve the latest active/inactive state across
  transcript compaction or pruning. Slice 144 adds explicit
  `deactivated_skill_names` policy input, validated as unique single-line names,
  so prompt-boundary deactivation subtracts from the transcript-derived active
  marker set without hidden clocks or bootstrap-local parsing. Slice 145 adds
  explicit `SkillExpiration` rows plus optional `evaluation_time`; expiration
  subtracts from the active marker set only when callers supply that evaluation
  time, keeping the renderer free of hidden wall-clock reads. Slice 146 wires
  the first runtime-owned source for these inputs: per-agent
  `agents.<name>.skills_deactivated` and `agents.<name>.skills_expirations`
  config, with bootstrap supplying the prompt-boundary evaluation time so the
  configured-route renderer stays clock-free. Slice 149 exposes
  `skill::SkillActivationEvent` plus
  `skill::skill_activation_events_from_transcript(...)`, making the
  transcript `skill.invoke` / `skill.deactivate` event scan a reusable
  `oran-skill` public primitive for future CLI/web/channel/automation runtime
  owners instead of bootstrap-local parsing.
- Section-4 cache semantics for activation policy.
  **Status (doc slice, 2026-06-02):** activation policy is now specified as
  prompt-boundary state. For the same loaded/allowed skill snapshot, transcript
  state, durable session activation rows, and explicit policy inputs, section 4
  must render byte-identical text.
  A changed active-marker set is an intentional cached-prefix invalidation for
  the next prompt only; it does not rewrite section 1 and does not move skill
  bodies into sections 1-6. Slices 144-145 implement the first explicit
  deactivation and expiration inputs on `skill::ActivationPolicy`. Expiration
  and durable runtime deactivation sources must either use explicit
  caller-provided times/events or durable runtime state; the renderer must not
  read hidden clocks. Exact
  `AgentPromptRunnerOptions::skills_catalog` bytes remain an embedder/test
  bypass of automatic loader and policy handling.
- `skill.invoke(name, inputs)` tool runs a loaded skill. **Status (slices
  137, 142, 148, and 149, 2026-06-04):** the built-in is registered in the default active
  catalog with `Capability::invoke_skill`, parses only `name` plus optional
  raw JSON `inputs`, delegates lookup through `DispatchContext::skill_invoke`,
  and returns the matched markdown body as ordinary `tool_result` text for the
  next provider iteration. The successful output now also carries a small
  `data_json` activation record (`kind=skill_activation`, `version=1`, `name`)
  so the next prompt can mark that skill active in section 4 without parsing
  model-visible body text. `oran-skill` now also exposes the transcript event
  extractor that turns successful activation/deactivation tool results into
  semantic `SkillActivationEvent` rows for any runtime owner that needs to
  persist or replay the latest state. When session memory is enabled,
  bootstrap persists those events as the latest durable session row after the
  turn succeeds. The tool layer owns parsing, permissions, audit, hooks,
  scheduler dispatch, and output caps; bootstrap owns the snapshot lookup so
  `oran-tool` does not depend on `oran-skill`.
- `skill.deactivate(name)` tool clears a loaded skill's active marker. **Status
  (slices 147-149, 2026-06-04):** the built-in is registered in the default active
  catalog with `Capability::deactivate_skill`, parses only `{"name": <string>}`,
  delegates through `DispatchContext::skill_deactivate`, and returns a short
  confirmation plus a versioned `data_json` deactivation record
  (`kind=skill_deactivation`, `version=1`, `name`). The prompt-boundary
  `skill::active_skills_from_transcript` scan nets `skill.invoke` activations
  against `skill.deactivate` deactivations in transcript order, so the most
  recent transcript event for a skill decides whether it stays active; the
  record stays out of sections (1)-(6) and travels only as a section-7
  tool-result `data_json`. When session memory is enabled, bootstrap persists
  the successful deactivation as the latest durable inactive row after the turn
  succeeds by consuming the shared `SkillActivationEvent` extractor. The tool
  layer owns parsing, permissions, audit, hooks, scheduler dispatch, and output
  caps; bootstrap owns the snapshot lookup so `oran-tool` does not depend on
  `oran-skill`.
- Hot-reload via filesystem watcher (`asio` + inotify on Linux). **Status
  (slice 138, 2026-06-01):** prompt-boundary hot reload is implemented for
  configured-route `AgentPromptRunner` instances with a `skills_directory`.
  Exact pre-rendered catalog bytes still bypass filesystem loading for tests and
  embedders.
- Per-agent skill enablement through `agents.<name>.skills_enabled`. **Status
  (slices 139-140, 2026-06-01):** `oran-config` parses the optional non-empty
  string array on each agent config, and `AgentPromptRunner` can select an
  `agents.<name>` entry via `AgentPromptRunnerOptions::agent_config_name` (or
  fall back to `permission_agent_name`). When the field is absent, all loaded
  workspace skills remain visible. When it is present, the runner filters both
  the compact section-4 catalog and the `skill.invoke` document snapshot to
  that allowlist before each prompt-boundary snapshot replacement. A
  disallowed skill name therefore behaves like any other unloaded skill at
  invocation time. Configured-route binary startup maps `--agent <name>` into
  the same agent selection, so operator-chosen agents now get their own skill
  allowlist on ordinary prompt and REPL runs.

## Scope (v1.1)

- Skill chaining: a skill can declare follow-up skills it expects to be invoked.
- Skill-specific tool subset: a skill can restrict which tools may be used while it's
  active.
- Durable skill activation policy beyond per-agent config inputs is now shipped
  for the session-memory path. Slice 146 added per-agent `skills_deactivated` /
  `skills_expirations` config (the first runtime-owned source), slice 147
  adds the permissioned `skill.deactivate` built-in (capability
  `deactivate_skill`): a successful call records a versioned `skill_deactivation`
  transcript result so the agent can drop an active skill mid-session without a
  config edit. `skill::active_skills_from_transcript` nets that result against
  prior `skill.invoke` activations in transcript order (most recent event wins),
  so the change lands at the next prompt boundary only. Slice 148 adds a
  session-store-backed activation record that overlays transcript-derived state
  and survives transcript compaction/pruning; slice 149 promotes the
  transcript event extraction to `oran-skill` so non-bootstrap runtime owners
  can persist the same event stream without copying parser logic. All sources
  preserve the section-4 cache semantics above.

## Scope (v2)

- Cross-runtime skill registry (a shared bucket of skills auto-pulled).
- Skill versioning + immutability (similar to immutable container images).

## Out Of Scope

- Skill marketplaces or remote skill repositories.
- Code-execution skills (a skill is markdown, not arbitrary code).

## Acceptance Criteria

1. A skill placed under `<workspace>/.orangutan/skills/release-note.md` appears in the
   agent's initial skill catalog snapshot. Add/update/remove changes are
   reflected on the next prompt after the filesystem event is visible; no
   restart is required.
2. `skill.invoke("release-note", { since: "v1.2", ... })` runs the skill and the
   agent produces output consistent with the skill template.
3. Removing the skill file from disk removes it from the next prompt's catalog
   without restart, while any already-running turn keeps its coherent snapshot.
4. Skill activation is observable via the `tool_after` hook with
   `tool_name = "skill.invoke"`, and the next prompt marks successful
   transcript-backed invocations as active in section 4 while the active turn's
   cached prefix remains unchanged.
5. Activation policy changes affect section 4 only at prompt boundaries. Given
   identical loaded/allowed skills and identical policy inputs, repeated renders
   produce byte-identical section-4 text. A changed active marker, deactivation,
   or expiry changes the section-4 content hash and cached prefix only for the
   next prompt, while invoked skill bodies remain conversation-tail tool-result
   text.
6. `tests/skill/` ≥ 80% coverage. Slices 142-146 add activation metadata,
   active-marker, and explicit activation/deactivation/expiration policy
   coverage, plus config-sourced deactivation/expiration; slice 147 adds
   transcript-event `skill.deactivate` coverage (`tests/skill`, `tests/tool`,
  `tests/core`); slice 148 adds durable session activation overlay coverage
  (`tests/skill`, `tests/storage`, `tests/memory`, `tests/bootstrap`), and
  slice 149 adds public activation-event extractor coverage in `tests/skill`.
   `tests/config` and `tests/bootstrap` cover the config source, the deactivate
   built-in, and runner integration.

## Design Doc Cross-References

- [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md)
- [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)

## Risks

- A misbehaving skill bloats the prompt — enforce a per-skill body size cap (default
  4 KiB).
- Skill file changes during a turn cause inconsistency — snapshot the skill body at
  invocation time.

## Validation

```sh
xmake build test-skill bench-skill
xmake run test-skill
xmake run bench-skill
xmake run test-bootstrap
```

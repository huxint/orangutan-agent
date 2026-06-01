# 0009 — Skills

## User Problem

Some prompts are reusable: "review a PR", "summarize a doc", "write a release note".
Encoding these as **skills** — short markdown templates with metadata — lets the agent
activate them on demand without bloating the system prompt.

## Scope (v1)

- `oran-skill::Loader` reading skills from `<workspace>/.orangutan/skills/<name>.md`.
  **Status (slices 136-138, 2026-06-01):** the loader snapshots existing
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
  preamble.
- `skill.invoke(name, inputs)` tool runs a loaded skill. **Status (slice 137,
  2026-06-01):** the built-in is registered in the default active catalog with
  `Capability::invoke_skill`, parses only `name` plus optional raw JSON
  `inputs`, delegates lookup through `DispatchContext::skill_invoke`, and
  returns the matched markdown body as ordinary `tool_result` text for the next
  provider iteration. The tool layer owns parsing, permissions, audit, hooks,
  scheduler dispatch, and output caps; bootstrap owns the snapshot lookup so
  `oran-tool` does not depend on `oran-skill`.
- Hot-reload via filesystem watcher (`asio` + inotify on Linux). **Status
  (slice 138, 2026-06-01):** prompt-boundary hot reload is implemented for
  configured-route `AgentPromptRunner` instances with a `skills_directory`.
  Exact pre-rendered catalog bytes still bypass filesystem loading for tests and
  embedders.

## Scope (v1.1)

- Skill chaining: a skill can declare follow-up skills it expects to be invoked.
- Skill-specific tool subset: a skill can restrict which tools may be used while it's
  active.
- Per-agent skill enablement (skills listed in `agent.<name>.skills_enabled`).

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
   `tool_name = "skill.invoke"`.
5. `tests/skill/` ≥ 80% coverage. Slice 138 adds watcher/snapshot refresh
   coverage; `tests/bootstrap` covers runner integration.

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

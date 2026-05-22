## [2026-05-22 11:04] | Task: Tool Write/Edit Max Bytes

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none — single-slice follow-up from `docs/STATUS.md`

### User Query

> 深度理清项目目标和架构, 明白项目当前的实际进展, 继续推进项目的实现.

### Changes Overview

- Areas: `oran-tool`, tool tests, tool/file-view docs, tech-debt tracker,
  prompt framework docs, deep-review lifecycle rule.
- Key actions: added optional `max_bytes` to `file.write` and `file.edit`
  with a 16 MiB default / hard ceiling; added focused boundary and oversize
  tests; cleaned the tracked `-Wmissing-field-initializers` warnings in
  `tests/tool/test_registry.cpp`; deleted the stale root deep-review
  artifact after absorbing its actionable items; refreshed the prompt
  framework routing; codified the deep-review artifact lifecycle in a
  new `docs/rules/deep-review.md` (versioned name + version stamp on
  creation, delete-in-same-slice on absorption, no `/tmp/...` paths in
  live current-state docs); stripped the pre-existing `/tmp/...` review
  references from `docs/STATUS.md` and `docs/exec-plans/tech-debt-tracker.md`
  (histories keep their references as immutable provenance); bumped the
  slice version to 34.

### Design Intent

The deep review called out unbounded mutation payloads on `file.write` and
`file.edit`. This slice keeps the cap at the tool boundary rather than
expanding the `oran-io` public API: the user-visible risk is the agent-facing
tool contract, and the existing 16 MiB `ReadTextOptions` default already gives
the right ceiling. Callers may lower `max_bytes` per invocation, but cannot
raise it above 16 MiB, so a mutation cannot create text that a later
`file.read` refuses to ingest.

The old root deep-review artifact was no longer a reliable source of truth:
slices 31-34 closed its high-priority findings, and the remaining follow-ups
now live in `docs/exec-plans/tech-debt-tracker.md` plus specs 0011-0018. The
prompt framework now points agents at the live rule/spec/status chain rather
than old review text.

The same slice also codifies the deep-review lifecycle in a new
`docs/rules/deep-review.md`: future review artifacts get a versioned name
(`docs/reviews/YYYY-MM-DD-slice<N>-<slug>.md`), a first-line stamp declaring
the target slice and absorption home, and are deleted in the same slice that
closes the last absorbed finding. The two pre-existing `/tmp/...` review
references in `docs/STATUS.md` and `docs/exec-plans/tech-debt-tracker.md` are
replaced with self-contained descriptions so a future agent on a fresh
checkout is not asked to read files that never existed in the repo. History
entries keep their `/tmp/...` references as immutable provenance.

### Files Modified

- `src/oran-tool/file_write.cpp`
- `src/oran-tool/file_edit.cpp`
- `include/oran/tool/builtins.hpp`
- `tests/tool/test_registry.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `orangutan-deep-review.md` (deleted; stale root review artifact)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/product-specs/0011-file-view-and-caching.md` — mutation cap contract.
- `docs/design-docs/tool-runtime.md` — shipped `max_bytes` behavior and
  warning cleanup note.
- `docs/ARCHITECTURE.md` — current tool inventory.
- `docs/QUALITY_SCORE.md` — test counts and tool-registry surface.
- `docs/STATUS.md` — slice 34, history pointer, latest tool test count,
  remaining P0 backlog.
- `docs/exec-plans/tech-debt-tracker.md` — removed the closed content-cap and
  test-warning bullets from the open backlog and made the absorbed review
  backlog self-contained.
- `docs/rules/prompt-design.md` — added the current prompt-framework map from
  invariant rule to implementation specs, agent-loop consumer, observability,
  and current-state docs.
- `CLAUDE.md` / `AGENTS.md` — prompt routing now links the full live prompt
  surface and marks old review notes as provenance only.
- `docs/agents/domain.md` — domain-doc consumption now rejects deleted review
  artifacts as live guidance.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — cross-links the
  fake-provider loop and first-loop observability specs.
- `docs/rules/deep-review.md` — new rule covering the versioned name, version
  stamp, absorption pipeline, and delete-on-close discipline for deep-review
  artifacts. Cross-linked from `docs/rules/README.md`, `CLAUDE.md` /
  `AGENTS.md` Module Routing + Working Posture, `docs/agents/domain.md`, and
  `docs/rules/docs-in-sync.md` (new row for deep-review absorption + deletion).
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build test-tool` — passes; warning cleanup confirmed.
  - `xmake run test-tool` — passes, 92 cases / 750 assertions.
- Tests added/changed: `file.write` boundary/oversize `max_bytes`,
  `file.edit` output-size `max_bytes`, malformed `max_bytes` matrices, and
  test-only warning cleanup.
- Bench impact: none; cap validation is not a hot-path algorithmic change.
- Compile-budget delta: not measured; only two `oran-tool` TUs and the existing
  monolithic `tests/tool/test_registry.cpp` changed.

### Follow-ups

- Tech-debt entries: remaining deep-review P0 items are transparent
  `Registry::entries_` hashing and JSON-schema validation at `Registry::add`.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`

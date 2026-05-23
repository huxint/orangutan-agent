## [2026-05-24 06:55] | Task: prompt active-tools config

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local xmake / GCC 16.1 C++26`
- Linked plan: none — this was a narrow slice under `docs/STATUS.md`.

### User Query

Continue implementing Orangutan v2 one slice at a time, with project docs read
first and docs kept in sync with code.

### Changes Overview

- Areas: `oran-config`, spec 0016 prompt-catalog prework, bootstrap version
  reporting, tests, docs.
- Key actions: added typed `runtime.prompt.active_tools` parsing for the future
  prompt builder, documented the default sentinel in `config.example.json`, and
  extended config tests / bench fixtures around the new shape.

### Design Intent

Spec 0016's prompt builder needs a config-owned selector before it can decide
which tool schemas render in section 2. This slice lands only that dependency:
`oran-config` accepts either `"defaults"` or an explicit tool-name allowlist and
stores it on `config::RuntimeConfig::prompt.active_tools`. The loader validates
JSON shape plus non-empty names, but it deliberately does not resolve names
against `tool::Registry`; config sits below `oran-tool`, and registry-aware
validation belongs to the prompt builder that will consume this data.

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `tests/config/test_config.cpp`
- `config.example.json`
- `bench/config/scenarios/loading.cpp`
- `bench/config/README.md`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/design-docs/agent-platform.md`
- `docs/design-docs/secrets-and-state.md`
- `docs/design-docs/tool-runtime.md`
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md`
- `docs/rules/prompt-design.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 69 snapshot, shipped config surface, and remaining
  0016 prompt-builder / promotion work.
- `docs/ARCHITECTURE.md` — `oran-config` inventory and configuration section
  now include `runtime.prompt.active_tools`.
- `docs/design-docs/agent-platform.md` — prompt assembly now names the config
  selector that will drive section 2.
- `docs/design-docs/secrets-and-state.md` — configuration status now lists the
  new `runtime.prompt.active_tools` field and its validation boundary.
- `docs/design-docs/tool-runtime.md` — deferred-tool status distinguishes the
  shipped config selector from future prompt-builder consumption.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — records the
  implemented parser contract and the remaining section-rendering criterion.
- `docs/rules/prompt-design.md` — section 2 wording now names the active-tool
  selector rather than implying every non-deferred registered tool is rendered.
- `docs/QUALITY_SCORE.md` — updated config status, config test counts, and the
  remaining prompt-builder follow-up.
- `docs/releases/feature-release-notes.md` — added the user-visible slice note.
- `bench/config/README.md` — notes that the loading fixture exercises the new
  prompt config block.

### Validation

- Commands run:
  - `xmake build test-config`
  - `xmake run test-config` — 28 cases / 207 assertions
  - `xmake build bench-config`
  - `xmake build orangutan`
  - `xmake run orangutan --prompt smoke` — reports `orangutan v2.0.0-slice69`
  - `git diff --check`
  - `make check-docs`
  - `scripts/check-status-fresh.sh`
  - `scripts/check-docs-sync.sh`
  - `xmake test` — 10 test targets passed
  - `make ci`
- Tests added/changed: `test-config` now covers `"defaults"`, explicit
  allowlists, empty explicit allowlists, non-object prompt blocks, unknown
  string sentinels, non-array active-tool values, non-string entries, and empty
  tool names.
- Bench impact: no new A/B scenario; config loading is startup-only. The
  existing `bench-config` loading fixture now includes `runtime.prompt.active_tools`
  so the parser path stays exercised.
- Compile-budget delta: one small addition in the existing `src/oran-config/config.cpp`
  TU plus public config structs; no new library or TU.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`

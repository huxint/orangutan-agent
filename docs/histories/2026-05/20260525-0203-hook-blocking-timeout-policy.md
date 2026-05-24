## [2026-05-25 02:03] | Task: Hook Blocking Timeout Policy

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `/home/huxint/projects/orangutan-refactor`
- Linked plan: none — this stayed within the existing spec-0015 sequencing.

### User Query

> Continue iterating the Orangutan v2 implementation, understand the docs before
> coding, keep docs in sync, and land one version as one commit.

### Changes Overview

- Areas: `oran-hook`, `oran-config`, `oran-bootstrap`, direct tool-dispatch audit
  metadata, and spec/status docs.
- Key actions: parsed the top-level `hooks.timeout_ms` policy, made
  `RuntimeAssembly` own the configured process hook bus, raced blocking hook sinks
  against that timeout, converted timeouts into `hook_timeout` veto decisions, and
  persisted timeout `elapsed_ms` in direct-dispatch audit metadata.

### Design Intent

Spec 0015 requires blocking sinks to be bounded before they can guard real tool
dispatch. This slice keeps the timeout policy at the hook bus boundary, not inside
individual tools: bootstrap configures the process bus once, the bus classifies a
slow sink as a veto, and `Registry::dispatch` reuses the existing slice-91
`blocked_by_hook` path so handler skipping, advisory failure events, and audit
metadata stay consistent with ordinary hook vetoes.

### Files Modified

- `include/oran/hook/{bus,decision}.hpp`
- `src/oran-hook/bus.cpp`
- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `include/oran/bootstrap/runtime_assembly.hpp`
- `src/oran-bootstrap/{bootstrap,runtime_assembly}.cpp`
- `src/oran-tool/audit_metadata.cpp`
- `tests/{hook,config,bootstrap,tool}/`
- `config.example.json`
- `docs/`

### Docs Updated In This PR

- `docs/STATUS.md` — slice 92 summary, focused test counts, tech-debt summary.
- `docs/ARCHITECTURE.md` — config/bootstrap/hook public-surface inventory.
- `docs/QUALITY_SCORE.md` — current test counts and area status.
- `docs/SECURITY.md` — hook timeout denial posture.
- `docs/design-docs/bootstrap-runtime.md` — `RuntimeAssembly` service bundle.
- `docs/design-docs/permissions-and-hooks.md` — timeout policy and failure mode.
- `docs/design-docs/secrets-and-state.md` — typed `hooks.timeout_ms` config field.
- `docs/design-docs/tool-runtime.md` — dispatch timeout/audit outcome wording.
- `docs/product-specs/0012-tool-scheduler-and-state.md` — scheduler vs. bus timeout boundary.
- `docs/product-specs/0015-blocking-hook-decisions.md` — AC6 status.
- `docs/exec-plans/tech-debt-tracker.md` — remaining hook follow-ups.
- `docs/releases/feature-release-notes.md` — slice release note.

### Validation

- Commands run:
  - `xmake run test-config` — 32 cases / 235 assertions.
  - `xmake run test-hook` — 30 cases / 207 assertions.
  - `xmake run test-bootstrap` — 57 cases / 224 assertions.
  - `xmake run test-tool` — 173 cases / 1739 assertions.
- Tests added/changed: hook bus timeout regression, config hook-policy parsing
  coverage, runtime assembly hook-bus wiring coverage, and direct-dispatch
  `blocked_by_hook` audit metadata coverage for timeout decisions.
- Bench impact: not measured; no new bench scenario because this is timeout policy
  and configuration wiring, not an alternative performance implementation.
- Compile-budget delta: not measured; public additions use `<chrono>` and forward
  declarations, with coroutine/timer machinery kept in `src/oran-hook/bus.cpp`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: existing 2026-05-18 hook row now narrows to the
  operator-prompt sink and the spec-0018 `hook_publish` audit-row writer.
- Linked release note: `docs/releases/feature-release-notes.md`.

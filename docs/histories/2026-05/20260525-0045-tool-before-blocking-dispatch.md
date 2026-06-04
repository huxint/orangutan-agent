## [2026-05-25 00:45] | Task: Tool-Before Blocking Dispatch

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local repository checkout`
- Linked plan: none — this stayed within the existing spec-0015 sequencing.

### User Query

> Continue iterating the Orangutan v2 implementation, understand the docs before
> coding, keep docs in sync, and land one version as one commit.

### Changes Overview

- Areas: `oran-hook`, `oran-tool`, `oran-permission`, audit metadata,
  hook benches, and spec/status docs.
- Key actions: added `HookDecisionTrace`, made direct `Registry::dispatch`
  consume blocking `tool_before` decisions, added audit outcomes for
  hook veto/rewrite, serialized hook decision metadata, added focused
  dispatch tests, and added blocking-publish bench scenarios.

### Design Intent

Spec 0015 says approval rendering and future policy hooks must not become
special cases in the agent loop. This slice makes the already-shipped
`publish_blocking` bus surface real at the first effectful boundary:
direct tool dispatch. Rewrites happen before workspace and permission
evaluation so hooks cannot bypass policy; vetoes become ordinary
permission-denied failures with durable audit evidence; and
`require_approval` reuses the existing broker path rather than inventing a
parallel approval mechanism.

### Files Modified

- `include/oran/hook/decision.hpp`
- `include/oran/hook/bus.hpp`
- `include/oran/permission/audit.hpp`
- `include/oran/storage/audit_repository.hpp`
- `include/oran/tool/registry.hpp`
- `src/oran-hook/bus.cpp`
- `src/oran-tool/{registry.cpp,audit_metadata.cpp,_impl/audit_metadata.hpp}`
- `tests/{hook,permission,tool}/`
- `bench/hook/`
- `docs/`

### Docs Updated In This PR

- `docs/STATUS.md` — slice 91 summary, focused test counts, tech-debt summary.
- `docs/ARCHITECTURE.md` — hook/tool/permission public-surface inventory.
- `docs/QUALITY_SCORE.md` — test counts and hook/tool/permission status.
- `docs/SECURITY.md` — hook-driven veto/rewrite security posture.
- `docs/design-docs/permissions-and-hooks.md` — blocking bus and dispatch consumer status.
- `docs/design-docs/tool-runtime.md` — canonical dispatch order.
- `docs/product-specs/0012-tool-scheduler-and-state.md` — direct-dispatch consumer status.
- `docs/product-specs/0015-blocking-hook-decisions.md` — v1 AC status.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — downstream boundary note.
- `docs/exec-plans/tech-debt-tracker.md` — remaining hook follow-ups.
- `docs/releases/feature-release-notes.md` — slice release note.

### Validation

- Commands run:
  - `xmake run test-hook` — 29 cases / 196 assertions.
  - `xmake run test-permission` — 89 cases / 426 assertions.
  - `xmake run test-tool` — 172 cases / 1722 assertions.
  - `xmake run bench-hook` — blocking publish scenarios built and ran
    (`publish_blocking_three_sinks_all_proceed` was marked unstable by
    nanobench on this local run, but the measured single-sink path stayed
    under the spec target).
  - `xmake test` — all 13 test targets passed.
- Tests added/changed: blocking publish trace assertions and direct dispatch
  veto/rewrite/require_approval coverage in `tests/tool/test_registry.cpp`.
- Bench impact: `bench-hook` now includes blocking publish fan-out and
  short-circuit scenarios.
- Compile-budget delta: not measured; no new public heavy include or target.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: existing 2026-05-18 hook row now narrows to timeout
  enforcement, the operator-prompt sink, and hook-publish audit rows.
- Linked release note: `docs/releases/feature-release-notes.md`.

## [2026-06-01 04:12] | Task: slice 129 — provider profile-cost calculation

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: local repo checkout, `xmake` release build
- Linked plan: none — this was a single-slice follow-up selected from the
  `STATUS.md` next-runtime-piece candidates.

### User Query

> Continue deeply understanding the project and iterating the execution plan before
> further implementation.

### Changes Overview

- Areas: config, provider route resolution, agent loop usage accounting, docs.
- Key actions:
  - Added optional `profiles.<name>.pricing` fields for USD-per-million-token
    input/output/cache pricing.
  - Preserved the pricing metadata on resolved provider `ModelTarget`s.
  - Computed missing `provider::Usage::cost_estimate` in `agent::Loop` before
    provider response hooks, usage aggregation, and trace writes observe the turn.
  - Preserved provider-supplied cost estimates when they are already present.

### Design Intent

The trace rollup reader from slice 127 could already sum recorded costs, but real
provider responses often expose token counts without cost. This slice keeps protocol
adapters dumb: pricing is non-secret profile metadata, route resolution carries it to
the loop, and the loop computes a missing estimate at the observation boundary where
hooks and trace rows already consume usage.

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `include/oran/provider/system.hpp`
- `src/oran-provider/route_resolver.cpp`
- `src/oran-agent/loop.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/config/test_config.cpp`
- `tests/provider/test_route_resolver.cpp`
- `tests/agent/test_loop.cpp`
- `config.example.json`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 129 snapshot, history pointer, next-slice routing, and
  focused validation counts.
- `docs/ARCHITECTURE.md` — config/provider/agent inventory now names profile pricing
  and loop-side cost estimation.
- `docs/QUALITY_SCORE.md` — updated touched test counts and provider/agent next steps.
- `docs/design-docs/api-portability.md` — records the shipped profile-price cost path.
- `docs/design-docs/agent-platform.md` — provider cost-awareness status now includes
  profile pricing and loop-side estimates.
- `docs/design-docs/bootstrap-runtime.md` — configured-route handoff now mentions
  pricing metadata carried through resolution.
- `docs/design-docs/secrets-and-state.md` — config docs include the optional profile
  pricing object.
- `docs/product-specs/0001-core-react-loop.md` — current bootstrap/loop scope includes
  pricing metadata and cost estimates.
- `docs/product-specs/0012-tool-scheduler-and-state.md` — cost-aware scheduling now
  builds on shipped profile pricing.
- `docs/product-specs/0018-first-loop-observability.md` — token/cost rollup status
  includes profile-priced trace rows.
- `docs/releases/feature-release-notes.md` — user-visible release note.

### Validation

- Commands run:
  - `xmake build test-config`
  - `xmake build test-provider`
  - `xmake build test-agent`
  - `xmake run test-config`
  - `xmake run test-provider`
  - `xmake run test-agent`
- Tests added/changed:
  - Config pricing parse and validation coverage.
  - Provider route pricing preservation coverage.
  - Agent-loop cost-estimate calculation and provider-supplied-cost preservation
    coverage.
- Bench impact: none; the calculation is constant-time per provider response.
- Compile-budget delta: no baseline update; no new third-party includes in public
  headers.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`

## [2026-05-24 07:15] | Task: prompt promotion state

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local workspace / xmake`
- Linked plan: none — `docs/STATUS.md` described this one-shot spec-0016 increment, so it stayed below the exec-plan threshold.

### User Query

Continue advancing the rewrite after the prompt-builder skeleton, keeping docs and code in sync.

### Changes Overview

- Areas: `oran-prompt`, prompt benches, prompt docs.
- Key actions: added `prompt::PromotionState`, exposed promotion snapshots to `prompt::BuilderInputs`, promoted snapshot names into the next active catalog, added unit coverage for LRU / TTL / validation semantics, and extended `bench-prompt` with promoted-catalog plus promotion-state scenarios.

### Design Intent

Spec 0016 needs session-local deferred-tool promotions before `oran-agent` can wire `tool.search` side effects into a turn loop. This slice lands the pure prompt-side state first: callers pass explicit `core::Time` values, snapshots are sorted for deterministic prompt bytes, and the builder only consumes a snapshot supplied by its future session owner. The implementation uses a small promotion-specific LRU + TTL table rather than `core::BoundedCache` because this state must enumerate live tool names and distinguish new promotions from refreshes; the policy still matches spec 0012's bounded-state inventory (16 entries, 24-hour TTL).

### Files Modified

- `include/oran/prompt/promotion_state.hpp`
- `include/oran/prompt/builder.hpp`
- `src/oran-prompt/promotion_state.cpp`
- `src/oran-prompt/builder.cpp`
- `tests/prompt/test_builder.cpp`
- `bench/prompt/`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice/history pointer, latest prompt test count, and next work updated.
- `docs/ARCHITECTURE.md` — `oran-prompt` row now includes promotion state.
- `docs/design-docs/agent-platform.md` — prompt assembly records promotion snapshots as the future loop input.
- `docs/design-docs/tool-runtime.md` — deferred-tool status now reflects prompt-side promotion state while keeping `tool.search` side effects future.
- `docs/product-specs/0012-tool-scheduler-and-state.md` — bounded-state inventory records the shipped promotion-state policy.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — builder input, status, acceptance, and validation sections updated for slice 71.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — future loop prompt step now names the shipped promotion snapshot surface.
- `docs/QUALITY_SCORE.md` — prompt test/bench counts and remaining gaps updated.
- `docs/releases/feature-release-notes.md` — release note added.

### Validation

- Commands run:
  - `xmake build oran-prompt`
  - `xmake build test-prompt`
  - `xmake build bench-prompt`
  - `xmake run test-prompt`
  - `xmake run bench-prompt`
- Tests added/changed: `test-prompt` now reports 10 cases / 98 assertions.
- Bench impact: `bench-prompt` now reports `prompt.build_default_active_set`, `prompt.build_explicit_subset`, `prompt.build_promoted_subset`, and `prompt.promotion_state_promote_snapshot`.
- Compile-budget delta: no new dependency or public heavy include; `oran-prompt` incremental build remained under the documented budget locally.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: existing `bench/oran-agent/prompt_cache_hit_rate.cpp` and `scripts/check-prompt-preamble` rows remain open until `oran-agent` exists.
- Linked release note: `docs/releases/feature-release-notes.md` (`prompt-promotion-state`).

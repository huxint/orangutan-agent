## [2026-05-24 07:25] | Task: agent session promotion

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local workspace / xmake`
- Linked plan: none — `docs/STATUS.md`'s next-intended slice already described this single-session spec-0016 increment, so it stayed below the exec-plan threshold.

### User Query

Continue advancing the rewrite after prompt-side promotion state, keeping docs and code in sync.

### Changes Overview

- Areas: `oran-agent`, agent tests, agent benches, prompt/tool docs, build hygiene.
- Key actions: added the first `oran-agent` target with a public `agent::SessionState`, observed successful `tool.search` structured output, promoted deferred matches into the session's `prompt::PromotionState`, ignored non-search and failed search outputs, rejected malformed successful `tool.search` data without mutating state, and added the first agent-owned prompt-cache stability bench.

### Design Intent

Spec 0016 needed the session owner between `tool.search` and `prompt::Builder`: `oran-prompt` already knew how to store and consume promotion snapshots, but no agent-owned type called `promote` after discovery. This slice lands only that narrow session state, not the full ReAct loop. `agent::SessionState` keeps the public header free of `nlohmann_json`, parses `tool.search`'s structured `Output::data_json` privately in the `.cpp`, and mutates promotions only after the whole successful payload validates, so malformed structured output cannot leave a partial prompt-state change. The bench fixture proves the rendered prompt prefix stays stable across changing conversation tails both before and after a promotion.

### Files Modified

- `include/oran/agent.hpp`
- `include/oran/agent/session_state.hpp`
- `src/oran-agent/session_state.cpp`
- `tests/agent/test_session_state.cpp`
- `bench/agent/`
- `xmake/targets.lua`
- `xmake/tests.lua`
- `xmake/bench.lua`
- `src/oran-bootstrap/bootstrap.cpp`
- `.gitignore`
- `xmake-requires.lock` (removed from Git tracking; local resolver output remains ignored)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice/history pointer, latest agent test count, bench status, and next work updated.
- `docs/ARCHITECTURE.md` — `oran-agent` inventory now reflects the shipped `SessionState` promotion owner while keeping the full loop planned.
- `docs/BUILD_SYSTEM.md` — target inventory now includes `oran-agent`, and the xmake lockfile policy now records that `xmake-requires.lock` is a local resolver artifact rather than committed reproducibility evidence.
- `docs/rules/libraries.md` — `nlohmann_json` ownership now includes the private `oran-agent` parser.
- `docs/QUALITY_SCORE.md` — test/bench summaries, agent runtime row, and supply-chain lockfile status updated.
- `docs/product-specs/0012-tool-scheduler-and-state.md` — bounded-state inventory now records the shipped SessionState owner.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — `tool.search` side effect and prompt-cache fixture status updated for slice 72.
- `docs/design-docs/agent-platform.md` — prompt assembly status now names `agent::SessionState`.
- `docs/design-docs/tool-runtime.md` — deferred-tool implementation now records the shipped agent session owner.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — loop step 3 now starts from the shipped `SessionState` side effect.
- `docs/rules/prompt-design.md` — enforcement section now points at the live `bench-agent` SessionState fixture.
- `docs/SUPPLY_CHAIN_SECURITY.md` and `docs/rules/investigation.md` — package-version guidance now points at `xmake/packages.lua` plus `docs/rules/libraries.md`, because `xmake-requires.lock` is ignored.
- `docs/exec-plans/tech-debt-tracker.md` — closed the specific prompt-cache bench row.
- `docs/releases/feature-release-notes.md` — release note added.
- `tests/README.md` — `tests/agent` marked live.
- `bench/README.md` — `bench/agent` marked live.

### Validation

- Commands run:
  - `xmake build oran-agent`
  - `xmake build test-agent`
  - `xmake run test-agent`
  - `xmake build bench-agent`
  - `xmake run bench-agent`
- Tests added/changed: `test-agent` now reports 3 cases / 23 assertions.
- Bench impact: `bench-agent` adds `agent.prompt_cache_no_promotions` (~54.4 us / fixture) and `agent.prompt_cache_after_promotion` (~63.1 us / fixture).
- Compile-budget delta: no budget row changed; `oran-agent` already had a documented 1.5 s / 3.0 s / 3.5 s per-TU category, and the new public headers keep JSON parser types out of the ABI.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: the `bench/oran-agent/prompt_cache_hit_rate.cpp` prompt row is closed; the stable preamble static grep remains open until the first full loop/preamble template lands.
- Linked release note: `docs/releases/feature-release-notes.md` (`agent-session-promotion`).

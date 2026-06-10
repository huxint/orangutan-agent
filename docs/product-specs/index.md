# Product Specs Index

Product specs describe **what the user gets** at each milestone. They are paired with
design docs (which describe *how*) and execution plans (which describe *the steps to
ship*). For per-track frontier / next-step / pre-dependency detail, see
[`../ROADMAP.md`](../ROADMAP.md).

| ID | Spec | Status |
| -- | ---- | ------ |
| 0001 | [Core ReAct loop](0001-core-react-loop.md) | v1 shipped — loop, tool dispatch, streaming, REPL (slices 75–129) |
| 0002 | [Tool registry + built-in tools](0002-tool-registry.md) | v1 shipped — registry pipeline + shipped built-ins (through slice 115) |
| 0003 | [Multi-platform channels](0003-multi-platform-channels.md) | in progress — foundation shipped (slice 226); adapters/routing next |
| 0004 | [Agent team collaboration](0004-agent-team.md) | drafted — no code yet |
| 0005 | [Memory system](0005-memory-system.md) | v1 shipped — session + long-term + retention (slices 130–196) |
| 0006 | [Automation engine](0006-automation.md) | in progress — library-level retention/cron/triggered shipped (slices 187–225); runtime ownership pending |
| 0007 | [Desktop App](0007-web-ui.md) | drafted — docs repivoted from web UI; no code yet |
| 0008 | [Permissions engine](0008-permissions.md) | v1 shipped — rules, signed approval broker, operator ask round-trip (through slice 96) |
| 0009 | [Skills](0009-skills.md) | v1 shipped — catalog / loader / invoke / activation-policy arc (slices 135–149) |
| 0010 | [Benchmark harness](0010-benchmark-harness.md) | in progress — per-lib buckets + `bench-compare` shipped; A-vs-B scenario backlog in tech-debt |
| 0011 | [File-view: range reads, change detection, caching](0011-file-view-and-caching.md) | v1 shipped — range reads, fingerprints, caches, watcher (through slice 58) |
| 0012 | [Tool scheduler + bounded runtime state](0012-tool-scheduler-and-state.md) | v1 shipped — bounded parallelism, path locks, loop wiring (slices 116–120) |
| 0013 | [Workspace + path policy](0013-workspace-and-path-policy.md) | v1 shipped (slice 55) — v1.1 ignore-predicate work pending |
| 0014 | [Structured tool output (`ToolOutput` v2)](0014-structured-tool-output.md) | v1 shipped — built-in migration + provider mapping (slices 60–67, 107) |
| 0015 | [Blocking hook decisions](0015-blocking-hook-decisions.md) | v1 shipped — blocking dispatch, timeout, ask bridge (slices 91–96) |
| 0016 | [Prompt + tool-catalog cache](0016-prompt-and-tool-catalog-cache.md) | v1 shipped — builder, promotion, cache hints, stability bench (slices 59–73) |
| 0017 | [Fake-provider-first agent loop](0017-fake-provider-first-agent-loop.md) | shipped — slices 74–77 |
| 0018 | [First-loop observability & trace](0018-first-loop-observability.md) | v1 shipped — trace rows, `--trace` inspector, rollups, retention (slices 78–150) |

## Conventions

- One spec per feature or workflow.
- Start with the **user problem**, not the implementation.
- State **acceptance criteria** as observable outcomes.
- Cross-link to the matching design doc, execution plan, and release notes.
- Specs may be split into v1 / v1.x / v2 sections when the work is staged.
- A spec stays "drafted" until at least one acceptance criterion has shipping evidence
  (history entry + release note + test).
- Status values: `drafted` (no shipping evidence) → `in progress` (some
  acceptance criteria shipped) → `v1 shipped` / `shipped` (the spec's v1
  scope is in the binary with tests). The slice that changes a spec's
  shipping state updates this column in the same commit.

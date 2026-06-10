## [2026-06-10 10:35] | Task: channel mock ingress and dispatch seam

### Execution Context

- Agent: `Claude Code`
- Base model: `Fable 5`
- Runtime: `Claude Code CLI`
- Linked plan:
  `docs/exec-plans/active/2026-06-09-channel-ingress-and-adapters.md`

### User Query

> Deeply understand the project architecture and current implementation
> progress, read all relevant documentation, then continue implementing the
> next slice.

### Changes Overview

- Areas: `oran-channel` (mock adapter + dispatch seam), `oran-bootstrap`
  (channel prompt bridge), build wiring, channel design/spec docs,
  status/roadmap/history/release tracking.
- Key actions: added `channel::MockChannel`, the first concrete in-process
  ingress adapter (caller `push_inbound`, awaiting `next_message()` over a
  bounded queue, lifecycle gating, recorded sends with deterministic
  receipts); added the `channel::ChannelPromptRunner` dispatch seam
  (`ChannelPromptRunRequest`/`Result`, `make_prompt_run_request`,
  `make_reply_message`, caller-owned `dispatch_one`); added bootstrap's
  `make_channel_agent_prompt_runner(...)`, which runs dispatched channel
  messages through `AgentPromptRunner` with per-conversation session
  continuity.

### Design Intent

This is milestone 2 of the channel ingress plan: the smallest concrete
adapter/routing boundary proving one external message can enter
`ChannelManager` and be handed toward the configured agent path. The mock
adapter ships inside `oran-channel` (not a separate `oran-channel-mock`
target) because it has zero platform/HTTP dependencies and exists precisely
so higher layers — bootstrap routing next, then the QQ port — can test
ingress without a network listener. A webhook adapter was deliberately not
started here: `oran-http` is client-only today, so webhook ingress would
drag a new HTTP-server surface into the same slice.

The dispatch seam mirrors the automation precedent
(`automation::AutomationPromptRunner` ↔ bootstrap factory): `oran-channel`
defines the `std::function` runner contract plus a caller-owned
`dispatch_one` step, and `oran-bootstrap` owns the concrete
`AgentPromptRunner`-backed factory. The channel library stays independent of
agent/bootstrap internals, and loop/cancellation policy stays out of the
foundation (Dependency Frontier #2). The bridge derives a stable per-
conversation `session_id` from scope/channel/conversation/agent keys, so
follow-up messages in one conversation reuse persisted transcript state
when session memory is enabled — same continuity model automation jobs use,
keyed by conversation instead of job.

Debugging note: the first version of the manager+mock test hung by
double-evaluating `REQUIRE((co_await manager.receive_one(...)))` — the
known Catch2 expression-decomposition double-eval; binding awaited results
to locals first (house idiom) fixed it.

### Files Modified

- `include/oran/channel.hpp`
- `include/oran/channel/mock.hpp`
- `include/oran/channel/dispatch.hpp`
- `src/oran-channel/mock.cpp`
- `src/oran-channel/dispatch.cpp`
- `include/oran/bootstrap.hpp`
- `include/oran/bootstrap/channel_prompt_runner.hpp`
- `src/oran-bootstrap/channel_prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/channel/test_mock.cpp`
- `tests/channel/test_dispatch.cpp`
- `tests/bootstrap/test_channel_prompt_runner.cpp`
- `bench/channel/main.cpp`
- `bench/channel/scenarios/mock_ingress.cpp`
- `bench/channel/README.md`
- `xmake/targets.lua`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumps the snapshot to slice 227, refreshes channel and
  bootstrap test counts, and points the next slice at bootstrap channel
  registration/config routing.
- `docs/ROADMAP.md` — moves the Channels row frontier to the shipped mock
  ingress + dispatch seam and names bootstrap routing as the next step.
- `docs/design-docs/channel-abstraction.md` — records the shipped mock
  adapter, dispatch seam, and bootstrap bridge boundary.
- `docs/product-specs/0003-multi-platform-channels.md` — adds slice 227 to
  "Shipped So Far".
- `docs/ARCHITECTURE.md` — updates the `oran-channel` and `oran-bootstrap`
  inventory rows (new surfaces; bootstrap now depends on `oran-channel`).
- `docs/QUALITY_SCORE.md` — refreshes channel/bootstrap test counts, the
  bench inventory, and the Channels row.
- `docs/exec-plans/active/2026-06-09-channel-ingress-and-adapters.md` —
  checks off the mock ingress milestone and records the decision log.
- `docs/releases/feature-release-notes.md` — adds the slice 227 user-facing
  release note.
- `bench/channel/README.md` — documents the new mock-ingress A-vs-B
  scenario.

### Validation

- Commands run:
  - `xmake build test-channel` / `xmake run test-channel`
  - `xmake build test-bootstrap` / `xmake run test-bootstrap`
  - `xmake build bench-channel` / `xmake run bench-channel`
  - `xmake -j$(nproc)` (full build)
  - `xmake test` (all 18 buckets passed)
  - `xmake run orangutan -- --help` (reports `2.0.0-slice227`)
  - `make ci`
- Tests added/changed:
  - Added `tests/channel/test_mock.cpp` and `tests/channel/test_dispatch.cpp`;
    `test-channel` now reports 24 cases / 186 assertions (was 8 / 65),
    including a cancel-aware `dispatch_one` case.
  - Added `tests/bootstrap/test_channel_prompt_runner.cpp` with the
    end-to-end mock-ingress → manager → `dispatch_one` →
    `AgentPromptRunner` → mocked-provider → reply proof; `test-bootstrap`
    now reports 139 cases / 1224 assertions (was 134 / 1160).
- Bench impact:
  - Added `bench-channel` `mock_ingress` scenario: direct runner invocation
    ~4.1 µs vs. full mock ingress dispatch ~38.5 µs per 16-message batch
    (~2.4 µs/message ≈ 415k msg/s locally — far above the spec 0003
    acceptance floor of 200 msg/s).
- Compile-budget delta:
  - Two new small `oran-channel` TUs and one new `oran-bootstrap` TU; no new
    third-party dependency. `oran-bootstrap` gains a downward dep on
    `oran-channel` (composition root wiring the interface layer).

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`

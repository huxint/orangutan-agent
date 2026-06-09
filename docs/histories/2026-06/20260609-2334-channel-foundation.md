## [2026-06-09 23:34] | Task: channel foundation

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan:
  `docs/exec-plans/active/2026-06-09-channel-ingress-and-adapters.md`

### User Query

> Stop blindly continuing automation, understand how many automation slices have
> already shipped, and move to a more valuable slice that lets other runtime
> surfaces couple in later.

### Changes Overview

- Areas: `oran-channel`, build/test/bench registration, channel design/spec
  docs, status/history/release tracking.
- Key actions: added the first `oran-channel` foundation library with
  `Capabilities`, normalized inbound/outbound envelopes, the abstract
  `Channel` adapter trait, and caller-owned `ChannelManager` registration,
  lifecycle, one-message fan-in, outbound send, and capability lookup APIs.

### Design Intent

Automation had already consumed 39 consecutive automation-track slices
(187-225, including bootstrap retention/job mapping slices feeding automation),
including a long cron/triggered/service run. The next better boundary is
external ingress: channel adapters need a stable seam before QQ, webhook,
desktop, and later daemon routing can couple into the runtime. This slice keeps
the foundation deliberately narrow. It does not port QQ, open an HTTP listener,
start a daemon loop, or route messages into `AgentPromptRunner`; it gives those
future slices a tested public library instead of another automation-internal
layer.

`ChannelManager::receive_one(...)` is explicit and caller-owned. That avoids
hiding long-running receive loops, cancellation policy, per-conversation
serialization, or bootstrap startup inside the foundation library.

### Files Modified

- `include/oran/channel.hpp`
- `include/oran/channel/channel.hpp`
- `include/oran/channel/manager.hpp`
- `src/oran-channel/manager.cpp`
- `tests/channel/main.cpp`
- `tests/channel/test_manager.cpp`
- `bench/channel/main.cpp`
- `bench/channel/scenarios/manager_fanin.cpp`
- `bench/channel/README.md`
- `xmake/targets.lua`
- `xmake/tests.lua`
- `xmake/bench.lua`
- `src/oran-bootstrap/bootstrap.cpp`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumps the snapshot to slice 226, points to the channel
  plan/history, and reframes the next slice around external ingress coupling
  instead of the automation plan's next internal service boundary.
- `docs/ARCHITECTURE.md` — updates the library inventory for the now-shipped
  `oran-channel` foundation and its actual dependency direction.
- `docs/design-docs/channel-abstraction.md` — records the shipped public API
  and explicit one-message fan-in boundary.
- `docs/product-specs/0003-multi-platform-channels.md` — records the shipped
  foundation and remaining adapter/bootstrap acceptance gaps.
- `docs/QUALITY_SCORE.md` — updates test/bench counts and the channel row.
- `docs/BUILD_SYSTEM.md` — records the new library/test/bench target.
- `tests/README.md` — marks `tests/channel/` live.
- `bench/README.md` — marks `bench/channel/` live and corrects the channel
  A-vs-B spotlight.
- `docs/exec-plans/active/2026-06-09-channel-ingress-and-adapters.md` —
  captures the multi-slice ingress/adapters plan.
- `docs/releases/feature-release-notes.md` — adds the slice 226 user-facing
  release note.

### Validation

- Commands run:
  - `xmake build oran-channel`
  - `xmake build test-channel`
  - `xmake run test-channel`
  - `xmake build bench-channel`
  - `xmake run bench-channel`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - Added `tests/channel/test_manager.cpp`.
  - Focused result: `test-channel` passed with 8 cases / 65 assertions.
- Bench impact:
  - Added `bench-channel` with direct append vs.
    `ChannelManager::receive_one(...)` fan-in comparison.
  - Local run reports `channel.direct_append` at about 0.55 us and
    `channel.manager_receive_one` at about 11.2 us per 16-message batch.
- Compile-budget delta:
  - One new library target and one implementation translation unit. No new
    third-party dependency and no dependency from channel into agent,
    bootstrap, or HTTP.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`

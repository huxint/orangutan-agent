## [2026-06-25 22:22] | Task: serve channel ingress dispatch

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: local workspace `/home/huxint/projects/orangutan-refactor`
- Linked plan: none; this extends the completed runtime-service owner with the
  channel daemon owner called out in `docs/design-docs/channel-abstraction.md`.

### User Query

> 继续推进项目的实现

### Changes Overview

- Areas: `oran-bootstrap`, `oran-channel`, channel/runtime docs.
- Key actions: added `bootstrap::serve_channels(...)`, wired `orangutan --serve`
  to start registered `config.channels[]` adapters, pump inbound messages into
  `ChannelManager`, dispatch through `channel::dispatch_one`, reply through the
  owning adapter, and stop/drain pumps on shutdown. Tightened
  `MockChannel::stop()` so pending `next_message()` waits unblock as cancelled,
  with `start()` reopening a fresh inbound queue for loopback reuse.

### Design Intent

The channel layer deliberately kept `ChannelManager` caller-owned: it can
register adapters and fan in one explicit `receive_one(...)`, but it does not
hide a background loop. The first correct owner is the long-lived daemon boundary
that owns cancellation and shutdown. This slice puts that owner in `--serve`,
beside the watcher, automation, and scheduler-reaping concerns.

The pump shutdown path is explicit because `asio::cancellation_signal` is not a
sticky stop flag. `serve_channels` marks pumps stopping, calls `stop_all()` to
wake adapter-owned receives, emits each child cancellation signal, and drains a
completion channel before returning so no spawned pump outlives its borrowed
`ChannelManager`.

### Files Modified

- `include/oran/bootstrap/serve.hpp`
- `src/oran-bootstrap/serve.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-channel/mock.cpp`
- `tests/bootstrap/test_serve.cpp`
- `tests/channel/test_mock.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/channel-abstraction.md` — records slice 256 as the first
  background channel fan-in owner while keeping `ChannelManager` itself
  caller-owned.
- `docs/product-specs/0003-multi-platform-channels.md` — adds the shipped
  `--serve` channel ingress/dispatch slice and remaining gaps.
- `docs/design-docs/bootstrap-runtime.md` — updates service-mode ownership and
  shutdown semantics.
- `docs/ARCHITECTURE.md` — refreshes `oran-channel` and `oran-bootstrap`
  inventory entries.
- `docs/ROADMAP.md` — moves the channel/agent frontier to slice 256 and leaves
  QQ real-smoke as credential-blocked.
- `docs/QUALITY_SCORE.md` and `docs/STATUS.md` — refresh test counts and the
  slice frontier.
- `docs/releases/feature-release-notes.md` — adds the user-visible release row.

### Validation

- Commands run:
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap "[serve]"`
  - `xmake build test-channel`
  - `build/linux/x86_64/release/test-channel "[mock]"`
  - `xmake run test-channel`
  - `xmake run test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
  - `xmake test`
- Tests added/changed:
  - `test-channel` now reports 26 cases / 205 assertions.
  - `test-bootstrap` now reports 172 cases / 1664 assertions.
  - Added direct `serve_channels` happy-path, parent-cancel, and null-runner
    coverage; added a `MockChannel::stop()` pending-receive regression.
  - Full `xmake test` reports 19 / 19 buckets passed.
- Bench impact: no new bench; this is daemon lifecycle/correctness work, not a
  measured hot path.
- Compile-budget delta: not measured; no new heavy public includes beyond the
  existing bootstrap/channel surfaces.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: no new row; remaining work is already represented by
  ROADMAP next steps (triggered-work ingress, per-conversation channel ordering,
  QQ real-smoke credentials).
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`

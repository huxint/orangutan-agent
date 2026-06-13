## [2026-06-13 23:21] | Task: QQ Inbound Channel Adapter

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: Codex CLI/API workspace
- Linked plan: `docs/exec-plans/active/2026-06-10-channel-qq-port.md`

### User Query

> Continue the long-running slice work after reading the project docs and current
> state; pick the most valuable current slice, keep docs in sync, validate, and
> commit with a proper message.

### Changes Overview

- Areas: `oran-channel-qq`, QQ port milestone 3, channel trait receive path.
- Key actions: added `<oran/channel-qq/channel.hpp>` with `qq::QqChannel`,
  `QqChannelOptions`, `QqDispatchNormalizationOptions`, and
  `normalize_gateway_dispatch(...)`; implemented C2C/group gateway
  dispatch-to-`channel::InboundMessage` normalization; wired
  `QqChannel::next_message()` over `GatewayTransport::next_dispatch()`; moved
  `oran-channel-qq` onto the real `oran-channel` dependency; bumped the binary
  slice tag to `2.0.0-slice233`.

### Design Intent

Slice 232 deliberately stopped at `GatewayTransport`; this slice takes the next
small step across the generic trait boundary without mixing in QQ's harder
outbound constraints. Inbound normalization is pure and testable, so the same
shape can later serve the webhook path. `QqChannel::send(...)` returns
`capability_not_granted` for now because text/passive reply sending needs its
own focused slice for C2C/group endpoint selection, `msg_id`/`msg_seq`, receipt
parsing, and quota behavior. This keeps the first trait-adapter slice reviewable
while making the dependency graph honest: `oran-channel-qq` now depends on
`oran-channel`.

### Files Modified

- `include/oran/channel-qq/channel.hpp`
- `include/oran/channel-qq/gateway_transport.hpp`
- `include/oran/channel-qq.hpp`
- `src/oran-channel-qq/channel.cpp`
- `tests/channel-qq/test_channel.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `xmake/targets.lua`
- `scripts/check-deps.sh`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped slice/current history, refreshed channel-qq test
  counts, and set the next intended QQ slice to outbound text/passive reply.
- `docs/ROADMAP.md` — moved the Channels frontier to slice 233 and recorded the
  next outbound-builder step.
- `docs/design-docs/channel-abstraction.md` — documented the shipped
  `QqChannel` receive boundary and normalization contract.
- `docs/product-specs/0003-multi-platform-channels.md` — added the slice-233 QQ
  shipped-so-far entry.
- `docs/exec-plans/active/2026-06-10-channel-qq-port.md` — split milestone 3
  into inbound, outbound, and registration sub-slices and recorded the decision.
- `docs/ARCHITECTURE.md` — updated the `oran-channel-qq` inventory row and
  dependencies.
- `docs/BUILD_SYSTEM.md` — documented the new gated `oran-channel` dependency.
- `docs/QUALITY_SCORE.md` — refreshed `oran-channel-qq` test counts and Channels
  status.
- `docs/releases/feature-release-notes.md` — added the user-visible slice note.

### Validation

- Commands run:
  - `xmake f --channel_qq=y && xmake build test-channel-qq`
  - `xmake build test-channel-qq && xmake run test-channel-qq`
  - `xmake build bench-channel-qq && xmake run bench-channel-qq`
  - `xmake f --channel_qq=n && xmake build orangutan && xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed: `tests/channel-qq/test_channel.cpp` adds six cases for
  C2C normalization, group normalization and mention stripping, unsupported
  dispatch skipping, malformed payload errors, lifecycle/deferred outbound
  behavior, and a loopback gateway `QqChannel::next_message()` proof.
- Bench impact: no new bench scenario; `bench-channel-qq` still builds and runs
  the existing API-normalization and gateway-consume comparisons.
- Compile-budget delta: one small `src/oran-channel-qq/channel.cpp` TU behind
  `--channel_qq=y`; no default-build adapter code is compiled.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none; milestone 3b remains in the active QQ-port plan.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`.

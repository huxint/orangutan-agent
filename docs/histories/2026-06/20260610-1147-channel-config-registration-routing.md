## [2026-06-10 11:47] | Task: channel config registration and agent routing

### Execution Context

- Agent: `Claude Code`
- Base model: `Fable 5`
- Runtime: `Claude Code CLI`
- Linked plan:
  `docs/exec-plans/completed/2026-06-09-channel-ingress-and-adapters.md`
  (milestone 3; plan closed in this slice with milestone 4 spun off to
  `docs/exec-plans/active/2026-06-10-channel-qq-port.md`)

### User Query

> Continue to finish implementing the channel-ingress plan, and archive it
> when done.

### Changes Overview

- Areas: `oran-config` (typed `channels[]` block), `oran-bootstrap`
  (channel registration + routed prompt runner), channel design/spec docs,
  plan lifecycle (ingress plan closed + archived, QQ-port plan created),
  status/roadmap/history/release tracking.
- Key actions: added `config::ChannelConfig` parsing (`id`, `kind`,
  `agent_key`, `inbound_capacity`; unique-id and strictness validation);
  added `bootstrap::register_configured_channels(...)` (builds and registers
  buildable adapters — only `"mock"` today — into a caller-owned
  `ChannelManager`, skipping and reporting unknown kinds, returning
  non-owning mock handles) and
  `bootstrap::make_routed_channel_prompt_runner(...)` (one
  `make_channel_agent_prompt_runner` bridge per distinct configured agent;
  requests dispatch by `channel_id`; unknown ids fail `not_found`).

### Design Intent

This is the channel-ingress plan's milestone 3: config-authored channels
become registered adapters, and inbound messages route to their configured
agents. The mapping mirrors the cron-config precedent — `oran-config` owns
typed JSON shape, bootstrap owns construction — and registration stays
caller-owned: no adapter is started, no receive loop is spawned, and
`bootstrap::run` does not consume `channels[]` yet (daemon/service ownership
is Dependency Frontier #2).

The routed runner is itself a `channel::ChannelPromptRunner`, so it plugs
into the existing `dispatch_one(...)` seam unchanged. Channels sharing an
agent share one bridge; per-channel session identity is preserved because
the slice-227 bridge derives session ids from the request's channel id. The
registration report returns non-owning `MockChannel*` handles because the
manager owns registered adapters and exposes no lookup — tests and future
loopback wiring need the push/observe surface.

With milestone 3 shipped, the plan's remaining milestone (QQ port) is a
platform-specific multi-slice migration that
`design-docs/channel-abstraction.md` assigns to a dedicated plan, so this
slice creates that plan and closes/archives the ingress plan per
`PLANS_GUIDE.md` ("close them; open a follow-up if needed").

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `include/oran/bootstrap.hpp`
- `include/oran/bootstrap/channel_ingress.hpp`
- `src/oran-bootstrap/channel_ingress.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/config/test_config.cpp`
- `tests/bootstrap/test_channel_ingress.cpp`
- `config.example.json`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 228 snapshot, refreshed counts, active-plan list
  (ingress → QQ-port), next slice.
- `docs/ROADMAP.md` — Channels row frontier/next step now points at the
  QQ-port plan.
- `docs/design-docs/channel-abstraction.md` — records the shipped
  config-authored registration/routing layer.
- `docs/design-docs/secrets-and-state.md` — `channels[]` is now a typed
  block; documents the registration/routing boundary.
- `docs/product-specs/0003-multi-platform-channels.md` — slice 228 shipped;
  the v1 "per-channel config" scope line closes for buildable adapters.
- `docs/ARCHITECTURE.md` — `oran-config` and `oran-bootstrap` rows gain the
  slice-228 surfaces.
- `docs/QUALITY_SCORE.md` — config/bootstrap counts and the Channels row.
- `docs/exec-plans/completed/2026-06-09-channel-ingress-and-adapters.md` —
  milestone 3 recorded; plan closed with a closing note and moved from
  `active/` to `completed/`.
- `docs/exec-plans/active/2026-06-10-channel-qq-port.md` — new dedicated
  QQ-port plan (milestones, risks, validation, gating decision).
- `docs/releases/feature-release-notes.md` — slice 228 release note.
- `config.example.json` — `channels[]` now shows the typed shape.

### Validation

- Commands run:
  - `xmake build test-config` / `xmake run test-config`
  - `xmake build test-bootstrap` / direct `[channel_ingress]` run
  - `xmake -j$(nproc)` (full build)
  - `xmake test` (all 18 buckets passed)
  - `xmake run orangutan -- --help` (reports `2.0.0-slice228`)
  - `make ci`
- Tests added/changed:
  - `tests/config/test_config.cpp` gains channel parsing/validation cases;
    `test-config` now reports 54 cases / 501 assertions (was 51 / 468).
  - Added `tests/bootstrap/test_channel_ingress.cpp` (6 cases / 80
    assertions): registration + skip reporting, capacity flow, per-channel
    agent routing with overlays, per-channel session independence, option
    validation, and the end-to-end config→adapter→agent→reply proof;
    `test-bootstrap` now reports 145 cases / 1304 assertions (was 139 /
    1224).
- Bench impact: none — routing adds one map lookup over the slice-227
  dispatch path already covered by `bench-channel`'s mock-ingress scenario;
  no plausible perf question to resolve.
- Compile-budget delta: one new `oran-bootstrap` TU and config-parser
  additions inside the existing TU; no new third-party dependency, no new
  targets.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`

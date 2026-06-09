# Channel Ingress And Adapters

## Goal

Land the channel interface path that lets external chat/webhook messages reach
the existing agent runtime without making adapters depend on agent internals.
The plan starts with the `oran-channel` foundation library, then adds concrete
mock/webhook/QQ adapter and bootstrap routing slices.

## Scope

- In scope:
- Ship `oran-channel` with the public `Channel` trait, capability matrix,
  inbound/outbound envelopes, and caller-owned `ChannelManager`.
- Keep the foundation independent of `oran-agent`, `oran-bootstrap`, and
  concrete HTTP adapters.
- Add tests and a bench bucket with the foundation library.
- Later slices add mock/webhook adapter coverage, config mapping, bootstrap
  registration, and agent/orchestration dispatch.
- Out of scope:
- Porting QQ in this foundation slice.
- Starting a daemon, background channel loop, or automatic bootstrap routing in
  this foundation slice.
- Adding HTTP listener/server code before the webhook adapter slice.

## Context

- Relevant docs:
- `docs/STATUS.md`
- `docs/design-docs/channel-abstraction.md`
- `docs/product-specs/0003-multi-platform-channels.md`
- `docs/design-docs/module-boundaries.md`
- `docs/rules/docs-in-sync.md`
- Relevant code paths:
- `include/oran/channel.hpp`
- `include/oran/channel/channel.hpp`
- `include/oran/channel/manager.hpp`
- `src/oran-channel/manager.cpp`
- `tests/channel/`
- `bench/channel/`
- Constraints:
- `oran-channel` is an interface-layer foundation and may depend only downward
  on `oran-core` and `oran-async`.
- Public headers must remain stdlib-shaped and keep heavy adapter/HTTP details
  out of the foundation surface.
- Compile-budget impact:
- One small new library target, one implementation translation unit, one test
  bucket, and one bench bucket. No third-party dependency is added.

## Risks

- Risk: the foundation accidentally becomes a scheduler or dispatcher.
  Mitigation: the first manager exposes explicit `receive_one(...)` fan-in and
  caller-owned lifecycle/send APIs only; automatic long-running receive loops
  stay for a later owner.
- Risk: adapters leak platform specifics into the agent runtime. Mitigation:
  keep platform-specific details behind `Channel` and `Capabilities`; the
  agent/dispatcher consumes normalized envelopes.
- Risk: another long internal workstream forms before a real user entrypoint is
  usable. Mitigation: after the foundation, prioritize a mock/webhook adapter
  plus bootstrap routing before optional adapter polish.

## Milestones

1. **Foundation library.** Ship `oran-channel`, public envelopes/trait,
   `ChannelManager`, tests, bench, and docs. Shipped in slice 226.
2. **Mock/webhook ingress.** Add the smallest concrete adapter path that can
   accept one inbound message and exercise dispatch through a mocked provider.
3. **Bootstrap routing.** Map config-authored channels into registered
   adapters and route inbound messages to configured agents.
4. **QQ port.** Port QQ behind the same trait after the generic ingress path is
   verified.

## Validation

- Commands:
- `xmake build oran-channel`
- `xmake build test-channel`
- `xmake run test-channel`
- `xmake build bench-channel`
- `xmake build orangutan`
- `xmake run orangutan -- --help`
- `make ci`
- Manual checks:
- Confirm `oran-channel` does not depend on `oran-agent`, `oran-bootstrap`, or
  `oran-http`.
- Confirm no automatic channel loop or bootstrap startup was introduced in the
  foundation slice.
- Bench comparison:
- `bench-channel` compares direct append with
  `ChannelManager::receive_one(...)` fan-in through `async::Channel`.

## Progress Log

- [x] 2026-06-09 23:34 +0800: Shipped slice 226 foundation library with
  channel envelopes, `Channel`, `ChannelManager`, `test-channel`, and
  `bench-channel`.
- [ ] Add the first concrete mock/webhook ingress adapter.
- [ ] Wire bootstrap channel registration and agent dispatch.
- [ ] Port QQ behind the shipped trait.

## Decision Log

- 2026-06-09: Pivoted away from the active automation plan after 39
  consecutive automation-track slices (187-225). The next highest-value boundary is an
  external ingress seam that lets future adapters couple into the agent runtime
  without pulling agent/bootstrap code into adapter libraries.
- 2026-06-09: `ChannelManager` starts with explicit `receive_one(...)` rather
  than spawning background adapter receive loops. This keeps lifecycle,
  cancellation, per-conversation serialization, and daemon ownership out of the
  foundation slice.

## Linked Artifacts

- Related design doc: `docs/design-docs/channel-abstraction.md`
- Related product spec: `docs/product-specs/0003-multi-platform-channels.md`
- History entry:
  `docs/histories/2026-06/20260609-2334-channel-foundation.md`
- Release note: `docs/releases/feature-release-notes.md`

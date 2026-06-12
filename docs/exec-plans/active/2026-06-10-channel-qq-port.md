# Channel QQ Port

## Goal

Port the legacy QQ adapter (`/home/huxint/projects/orangutan/src/channel/qq/`,
~3.6 kLoC) onto the shipped v2 `channel::Channel` trait as the optional
`oran-channel-qq` library, so a configured QQ bot account can exchange
messages with a configured agent through the existing
`ChannelManager` → `dispatch_one` → routed-prompt-runner path. Spun off from
[`../completed/2026-06-09-channel-ingress-and-adapters.md`](../completed/2026-06-09-channel-ingress-and-adapters.md)
milestone 4 per `design-docs/channel-abstraction.md` ("QQ Adapter
Migration"), which reserves a dedicated plan for the port.

## Scope

- In scope:
- New gated `oran-channel-qq` library (`option("channel_qq")`, default off
  until the port is validated) depending on `oran-channel` + `oran-http` +
  `oran-core` + `oran-async` only.
- QQ API client rewritten over `oran-http::Client` (no direct libcurl), with
  OAuth/token refresh isolated in a `TokenStore`.
- WebSocket-gateway receive transport (Hello/Identify/heartbeat/Resume) with
  reconnect backoff behind `Channel::next_message()`. (QQ has no long-poll;
  the gateway — or an HTTP webhook — is the only inbound transport. See
  `docs/references/messaging-platform-apis.md`.)
- Outbound send + message builder behind `Channel::send(...)`, with an
  honestly-filled `Capabilities` matrix.
- Config: `channels[].kind == "qq"` fields (credentials env names only — no
  secret values in config), bootstrap registration through the existing
  `register_configured_channels(...)` seam.
- Mock-HTTP-server integration tests (`tests/channel-qq/`) plus a bench
  bucket per C12.
- Out of scope:
- Attachments / media beyond text for the first slices (lands as a later
  milestone via a shared `AttachmentCache` abstraction).
- The legacy approval-keyboard interactive surface (revisit after the
  generic approvals-via-channel story).
- Background receive loops / daemon ownership (Dependency Frontier #2).
- Real-credential tests in CI (nightly opt-in env-var gate only).

## Context

- Relevant docs:
- `docs/design-docs/channel-abstraction.md` ("QQ Adapter Migration", "New
  Adapter Recipe")
- `docs/product-specs/0003-multi-platform-channels.md` (acceptance criterion
  2 and the QQ migration notes)
- `docs/references/orangutan-legacy-audit.md`
- `docs/rules/libraries.md` (no new third-party dependency expected)
- Relevant code paths:
- Legacy reference: `/home/huxint/projects/orangutan/src/channel/qq/`
  (`qq-api-client`, `qq-transport`, `qq-channel*`, `qq-message-builder`,
  `reconnect-backoff`) — reference only, not code to copy.
- v2 seams: `include/oran/channel/{channel,manager,dispatch,mock}.hpp`,
  `include/oran/bootstrap/{channel_ingress,channel_prompt_runner}.hpp`,
  `include/oran/http/client.hpp`.
- Constraints:
- Adapter stays behind the trait; no platform types leak into `oran-channel`
  or the agent runtime.
- Public headers stay C6-clean; curl/HTTP details live in `src/`.
- Compile-budget impact: one new gated library (`oran-channel-*` row already
  budgeted at 1.2 s median / 3.0 s hard cap per TU); disabled builds compile
  zero adapter code.

## Risks

- Risk: QQ API drift since the legacy client was written. Mitigation: first
  slice validates request/response shapes offline against recorded fixtures;
  a manual smoke test with real credentials gates enabling the option by
  default.
- Risk: token refresh races under the single-executor model. Mitigation:
  serialize refresh through a strand; test with the mock HTTP server's
  delayed responses.
- Risk: the WebSocket-gateway receive loop hides a background loop. Mitigation:
  `next_message()` stays one-resume-per-call (surfacing one gateway dispatch
  frame per resume); the persistent connection/heartbeat loop ownership stays
  with the future runtime-service owner.

## Milestones

1. **API client + TokenStore.** Offline request building/response decoding
   over `oran-http::Client` against a mock server; OAuth refresh isolated.
2. **Receive transport.** WebSocket-gateway `next_message()`
   (Hello/Identify/heartbeat/Resume) with reconnect backoff and
   cancel-awareness. (QQ offers no long-poll; gateway or webhook only.)
3. **Channel adapter.** `qq::QqChannel` implementing the trait end-to-end
   (send + message builder + capabilities), registered through
   `register_configured_channels(...)` behind `kind == "qq"` and
   `option("channel_qq")`.
4. **Round-trip acceptance.** Spec 0003 criterion 2: inbound QQ message →
   configured agent → reply sent back, observed via `audit.db` (mock server
   in CI; real credentials in a manual/nightly gate).
5. **Attachments (later).** Shared `AttachmentCache` + media capabilities.

## Validation

- Commands:
- `xmake f --channel_qq=y && xmake build oran-channel-qq`
- `xmake build test-channel-qq && xmake run test-channel-qq`
- `xmake build bench-channel-qq`
- `xmake f --channel_qq=n && xmake build orangutan` (zero adapter code
  linked)
- `make ci`
- Manual checks:
- No `curl/curl.h` or platform headers in any public header.
- Disabled option compiles no adapter TU.
- Secrets only as env-var names in config; never logged (C5).

## Progress Log

- [x] 2026-06-10: Plan created at the channel-ingress plan's close; no code
  yet.
- [x] Milestone 1: API client + TokenStore — slice 229
  ([`../../histories/2026-06/20260610-1325-channel-qq-api-client.md`](../../histories/2026-06/20260610-1325-channel-qq-api-client.md)).
  Gated `oran-channel-qq` target plus `test-channel-qq` (21 cases / 129
  assertions against a scripted loopback HTTP server) and
  `bench-channel-qq`; request/response shapes validated offline.
- [~] Milestone 2: receive transport.
  - [x] Milestone 2a: pure gateway protocol/session state machine — slice 230
    ([`../../histories/2026-06/20260612-1631-channel-qq-gateway-protocol.md`](../../histories/2026-06/20260612-1631-channel-qq-gateway-protocol.md)).
    `qq::GatewaySession` (`consume(frame_json)` → `GatewayReaction`,
    Identify-vs-Resume, seq/session-id continuity,
    `build_identify`/`build_resume`/`build_heartbeat`, `classify_close_code`),
    offline-tested (`test-channel-qq` +19 cases → 40 / 209) with no new
    dependency.
  - [ ] Milestone 2b: `wss://` network transport — a cancel-aware WebSocket
    boundary on `oran-http` driving `GatewaySession` behind
    `Channel::next_message()`, plus the heartbeat timer and reconnect backoff.
- [ ] Milestone 3: trait adapter + gated registration.
- [ ] Milestone 4: round-trip acceptance.

## Decision Log

- 2026-06-10: Spun off from the channel-ingress plan instead of extending it.
  The ingress plan's generic seams (trait, manager, dispatch, config
  registration, agent routing) shipped in slices 226-228; the QQ port is a
  multi-slice platform-specific migration with its own risk profile, which
  `design-docs/channel-abstraction.md` already assigns to a dedicated plan.
- 2026-06-10: `option("channel_qq")` defaults to **off** until milestone 4's
  round-trip acceptance passes against a mock server and a manual
  real-credential smoke test, despite the design doc sketching `default(true)`
  — an unvalidated network adapter must not be in default builds.
- 2026-06-10 (slice 229): the token-refresh race is serialized with the
  `oran-io` singleflight idiom (per-waiter timer, leader wake, shared
  outcome) instead of the strand the Risks section sketched — a strand
  cannot prevent double-refresh across `co_await` suspension points.
  Waiters whose leader was cancelled retry as the new leader so one
  cancelled caller does not poison concurrent callers.
- 2026-06-10 (slice 229): milestone 1 declares only the deps it consumes
  (`oran-core`, `oran-async`, `oran-http`); the `oran-channel` dep joins
  with milestone 3's trait adapter, keeping the dep graph honest
  (`scripts/check-deps.sh` gained the `channel-qq` interface-layer row).
- 2026-06-10 (slice 229): API bodies cross the public surface as serialized
  JSON strings (mirroring `tool::Output::data_json`) so the headers stay
  C6-clean; typed request/response shapes belong to the milestone-3
  adapter internals.
- 2026-06-12: corrected milestone 2's wording from "long-poll receive
  transport" to "WebSocket-gateway receive transport." A research pass
  against Tencent's own SDKs (`botgo`, `botpy`) and the
  `tencent-connect/openclaw-qqbot` plugin confirmed QQ offers **no**
  long-poll — inbound is a persistent WebSocket gateway
  (Hello/Identify/heartbeat/Resume) or an HTTP webhook (Ed25519 + op-13
  handshake). The `Channel::next_message()` trait method is unchanged
  (still one-resume-per-call); only the transport behind it was misnamed.
  The same pass also validated milestone-1 code (`QQBot ` auth prefix,
  string-or-int `expires_in`, case-insensitive `x-tps-trace-id`) as
  correct, and recorded the gateway opcode/close-code/intents details for
  milestone 2 in `docs/references/messaging-platform-apis.md`. Build the
  gateway before the webhook: it needs no public ingress and no Ed25519
  dependency.
- 2026-06-12 (slice 230): split milestone 2 into **2a** (this slice — the
  pure `qq::GatewaySession` protocol/session state machine) and **2b** (the
  `wss://` network transport). The codebase has no WebSocket primitive:
  `oran-http::Client` is body + SSE only, and the legacy transport ran raw
  `curl_ws_*` on a dedicated `std::thread`, which C2 (no `std::thread`) and
  C6 (no curl in headers) forbid. Doing the transport right means extending
  `oran-http` with a cancel-aware WebSocket boundary under `async::Runtime`
  — its own slice. 2a follows milestone 1's discipline of validating the
  protocol shapes offline first; it is also where the high-value SDK-grounded
  corrections live (4009 = the only resume-able close, 4013/4014 = intents,
  4914/4915 = fatal — facts the wiki and botpy get wrong), so locking them
  down under test before any network code is the right boundary.
- 2026-06-12 (slice 230): `consume` returns lifecycle frames (Hello, READY,
  RESUMED, heartbeat-ack) *before* the dispatch branch re-serializes the `d`
  object, so the read loop pays the `nlohmann` re-dump only for the
  non-lifecycle dispatches the inbound parser actually consumes (the bench
  confirms the heartbeat-ack hot path stays a decode-plus-no-op). The
  `GatewayReaction` is a struct of independent directives rather than a
  single enum because one Hello frame must both arm the heartbeat timer and
  request Identify/Resume.

## Linked Artifacts

- Related design doc: `docs/design-docs/channel-abstraction.md`
- Related reference: `docs/references/messaging-platform-apis.md`
  (code-grounded QQ gateway opcodes / close codes / intents / send shapes)
- Related product spec: `docs/product-specs/0003-multi-platform-channels.md`
- Predecessor plan:
  `docs/exec-plans/completed/2026-06-09-channel-ingress-and-adapters.md`
- History entries:
  - `docs/histories/2026-06/20260610-1325-channel-qq-api-client.md`
    (milestone 1, slice 229)
  - `docs/histories/2026-06/20260612-1631-channel-qq-gateway-protocol.md`
    (milestone 2a, slice 230)

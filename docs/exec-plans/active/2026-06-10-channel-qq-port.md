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
2. **Receive transport.** WebSocket-gateway driver under the future
   `QqChannel::next_message()` boundary (Hello/Identify/heartbeat/Resume)
   with reconnect backoff and cancel-awareness. (QQ offers no long-poll;
   gateway or webhook only.)
3. **Channel adapter.** `qq::QqChannel` implementing the trait end-to-end
   (send + message builder + capabilities), registered through
   `register_configured_channels(...)` behind `kind == "qq"` and
   `option("channel_qq")`.
4. **Round-trip acceptance.** Spec 0003 criterion 2: inbound QQ message →
   configured agent → reply sent back, observed via `audit.db`. Split into
   **4a** mock gateway/API coverage in CI and **4b** manual/nightly
   real-credential smoke before `channel_qq` can default on. 4b is split into
   the executable opt-in gate and the credentialed run result.
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
- [x] Milestone 2: receive transport.
  - [x] Milestone 2a: pure gateway protocol/session state machine — slice 230
    ([`../../histories/2026-06/20260612-1631-channel-qq-gateway-protocol.md`](../../histories/2026-06/20260612-1631-channel-qq-gateway-protocol.md)).
    `qq::GatewaySession` (`consume(frame_json)` → `GatewayReaction`,
    Identify-vs-Resume, seq/session-id continuity,
    `build_identify`/`build_resume`/`build_heartbeat`, `classify_close_code`),
    offline-tested (`test-channel-qq` +19 cases → 40 / 209) with no new
    dependency.
  - [x] Milestone 2b: `wss://` network transport.
    - [x] Milestone 2b-i: the `oran-http` WebSocket primitive — slice 231
      ([`../../histories/2026-06/20260613-1357-http-websocket-boundary.md`](../../histories/2026-06/20260613-1357-http-websocket-boundary.md)).
      `http::WebSocket` (`connect`/`receive`/`send_text`/`close` over libcurl
      connect-only mode), cancel-aware without a thread; shared libcurl RAII
      wrappers extracted to `src/oran-http/_impl/curl_common.hpp`; `test-http`
      +12 cases → 27 / 148, clean under ASan/UBSan, no new dependency.
    - [x] Milestone 2b-ii: drive `qq::GatewaySession` over `http::WebSocket`
      in `oran-channel-qq` — slice 232
      ([`../../histories/2026-06/20260613-2258-channel-qq-gateway-transport.md`](../../histories/2026-06/20260613-2258-channel-qq-gateway-transport.md)).
      `qq::GatewayTransport` owns the caller-driven persistent read loop under
      the trait adapter, races receives against the heartbeat timer over
      `async::sleep_for`, sends Identify/Resume/heartbeat payloads with tokens
      from `TokenStore`, applies the documented close-code/reconnect policy,
      and returns one non-lifecycle `GatewayDispatch` per `next_dispatch()`
      resume; `test-channel-qq` +5 cases → 45 / 249.
- [ ] Milestone 3: trait adapter + gated registration.
  - [x] Milestone 3a: inbound trait adapter — slice 233
    ([`../../histories/2026-06/20260613-2321-channel-qq-inbound-adapter.md`](../../histories/2026-06/20260613-2321-channel-qq-inbound-adapter.md)).
    `qq::QqChannel` now implements the receive side of the generic
    `Channel` trait over `GatewayTransport`; `normalize_gateway_dispatch(...)`
    maps C2C/group message dispatches into `channel::InboundMessage` with QQ
    conversation ids, reply references, origin/capabilities, and mention
    stripping. Outbound sends intentionally return `capability_not_granted`
    until the passive-reply builder lands; `test-channel-qq` +6 cases → 51 /
    309, and `oran-channel-qq` now depends on `oran-channel`.
  - [x] Milestone 3b: outbound text/passive reply over `ApiClient` — slice 234
    ([`../../histories/2026-06/20260613-2347-channel-qq-outbound-text.md`](../../histories/2026-06/20260613-2347-channel-qq-outbound-text.md)).
    `QqChannel::send(...)` now implements the smallest passive text reply path:
    it requires `reply_to_message_id`, maps `c2c:` / `group:` conversation ids
    onto the QQ v2 C2C/group message endpoints, serializes text replies with
    `content`, `msg_type:0`, `msg_id`, and a process-local `msg_seq`, and parses
    `id` / `msg_id` / `message_id` delivery receipts. The generic
    `make_reply_message(...)` now propagates the first inbound reply reference
    so `dispatch_one(...)` can preserve QQ inbound `msg_id`; `test-channel` +1
    case → 25 / 191 and `test-channel-qq` +4 cases → 55 / 344.
  - [x] Milestone 3c: bootstrap registration for `channels[].kind == "qq"` —
    slice 235
    ([`../../histories/2026-06/20260614-0015-channel-qq-bootstrap-registration.md`](../../histories/2026-06/20260614-0015-channel-qq-bootstrap-registration.md)).
    `oran-config` now validates QQ channel credential env-name and endpoint
    metadata, and enabled `--channel_qq=y` bootstrap builds register QQ
    channels through an internal owning wrapper that keeps `http::Client`,
    `TokenStore`, `ApiClient`, `GatewayTransport`, and `QqChannel` lifetimes
    together before handing a `Channel` pointer to `ChannelManager`. Default
    builds still skip/report QQ entries without linking adapter code; focused
    validation covers both build options.
- [ ] Milestone 4: round-trip acceptance.
  - [x] Milestone 4a: CI mock registered-path round-trip — slice 236
    ([`../../histories/2026-06/20260614-1507-channel-qq-registered-round-trip.md`](../../histories/2026-06/20260614-1507-channel-qq-registered-round-trip.md)).
    `test-bootstrap` now proves a configured QQ channel can be registered by
    bootstrap under `--channel_qq=y`, started by `ChannelManager`, receive one
    scripted WebSocket gateway C2C message, route through
    `make_routed_channel_prompt_runner(...)` to the configured agent, persist
    a trace turn through `audit.db`, and send the passive QQ reply back through
    a scripted v2 user-message API request. Default builds remain default-off
    and skip/report QQ entries without linking adapter code.
  - [ ] Milestone 4b: manual/nightly real-credential smoke over the same
    registered path before considering `channel_qq` default-on.
    - [x] Milestone 4b-i: hidden opt-in real-smoke gate — slice 237
      ([`../../histories/2026-06/20260614-1523-channel-qq-real-smoke-gate.md`](../../histories/2026-06/20260614-1523-channel-qq-real-smoke-gate.md)).
      Enabled `test-bootstrap` builds now include a hidden
      `[.][manual][channel-qq]` test that requires
      `ORAN_TEST_QQ_REAL_SMOKE=1` plus QQ app/gateway env vars, then drives
      the registered path against the real platform with a deterministic fake
      provider response: registration, start, one real operator message,
      routed prompt dispatch, trace/audit observation, passive reply send, and
      shutdown. Ordinary CI remains secret-free/network-free.
    - [ ] Milestone 4b-ii: run the hidden smoke with real QQ credentials and
      record the pass/fail evidence before considering `channel_qq`
      default-on.
      - [x] 4b-ii prep: gateway discovery helper and smoke setup reduction —
        slice 238
        ([`../../histories/2026-06/20260614-1551-channel-qq-gateway-discovery.md`](../../histories/2026-06/20260614-1551-channel-qq-gateway-discovery.md)).
        `oran-channel-qq` now parses `GET /gateway/bot` into typed
        `GatewayBotInfo` / `GatewaySessionStartLimit` values, and the hidden
        smoke uses that helper when `ORAN_TEST_QQ_GATEWAY_URL` is absent. This
        reduces operator setup for the credentialed run, but is not pass/fail
        evidence.

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
- 2026-06-13 (slice 231): split milestone 2b into **2b-i** (this slice — the
  `oran-http` WebSocket primitive) and **2b-ii** (the `GatewaySession` driver),
  the split slice 230's decision log already anticipated. Chose libcurl's
  connect-only WebSocket mode (`CURLOPT_CONNECT_ONLY = 2`) driven by the *multi*
  interface for cancel-awareness: the handshake runs in short non-blocking
  `curl_multi_perform` rounds between `async::sleep_for` ticks, and
  receive/send use connect-only's non-blocking `curl_ws_recv`/`curl_ws_send`
  (CURLE_AGAIN) suspending on asio socket readiness over a `dup` of curl's
  active socket — no `std::thread` (C2), no curl in headers (C6), and an idle
  connection costs no CPU-pool thread (critical under the default
  `cpu_workers = 1`). Two correctness fixes landed under test: (1) the
  connection lives in the *multi* handle's connection pool, so the `Impl` must
  keep the easy handle attached to a live multi for the connection's lifetime
  (removing it severed `CURLINFO_ACTIVESOCKET`); (2) `close()` now completes the
  RFC 6455 closing handshake (drain to peer close/EOF, bounded) so the close
  frame is delivered before the connection is torn down on destruction.
- 2026-06-13 (slice 232): completed **2b-ii** as a narrow
  `qq::GatewayTransport` driver instead of folding the full `QqChannel` trait
  adapter into the same slice. The driver returns `GatewayDispatch` from
  `next_dispatch()` and leaves platform dispatch-to-`InboundMessage` parsing,
  outbound sends, and bootstrap registration to milestone 3. That keeps the
  network/session/reconnect risk in one small surface while preserving the
  then-existing dependency boundary: `oran-channel-qq` still depended only on
  `oran-core`, `oran-async`, and `oran-http`; the trait-adapter slice was left
  to move the `oran-channel` dependency. The driver races
  `WebSocket::receive(...)` against the
  next heartbeat deadline with `async::sleep_for`, so no detached heartbeat
  task or background receive loop is introduced. Token acquisition failures
  surface immediately (auth/config errors are not swallowed as reconnects),
  while close 4004 invalidates the cached token before a fresh reconnect.
- 2026-06-13 (slice 233): split milestone 3 into **3a** (this slice —
  receive-side trait adapter), **3b** (outbound text/passive reply), and **3c**
  (bootstrap registration). The key dependency boundary has now moved:
  `oran-channel-qq` depends on `oran-channel` because `qq::QqChannel`
  implements the generic trait, and `scripts/check-deps.sh` documents that
  interface-layer sibling dependency. The adapter still does not register in
  bootstrap and still does not send; returning `capability_not_granted` from
  `send(...)` is deliberate so inbound normalization can be validated through a
  real `GatewayTransport` without mixing QQ's passive-reply quotas, `msg_seq`,
  and receipt decoding into the same slice.
- 2026-06-13 (slice 234): completed **3b** as a passive-text-only send path,
  not a general QQ outbound surface. Active push is documented as discontinued,
  so `send(...)` rejects messages without `reply_to_message_id` instead of
  silently attempting proactive sends. The `msg_seq` is a process-local
  monotonic counter for the first adapter slice; a durable per-`msg_id` quota /
  chunk tracker belongs with round-trip acceptance once bootstrap can assemble
  real configured channels. Receipt decoding accepts the three response id
  spellings seen across the QQ references, and malformed receipts surface as
  parsing errors without logging raw bodies.
- 2026-06-14 (slice 235): completed **3c** as registration and ownership
  assembly, not receive-loop ownership. `QqChannel` deliberately keeps its
  borrow-based constructor so platform tests and embedders can assemble the
  adapter pieces directly; bootstrap instead wraps it in a private `Channel`
  implementation that owns `http::Client`, `TokenStore`, `ApiClient`, and the
  concrete channel in declaration order. QQ channel config stores env-var
  names and endpoint URLs only; bootstrap resolves the app id / client secret
  environment variables only in enabled builds and keeps errors to non-secret
  context. `qq_gateway_url` is required in this slice because gateway discovery
  from `/gateway/bot` is asynchronous network work and belongs with milestone
  4's round-trip acceptance path.
- 2026-06-14 (slice 236): split milestone 4 into **4a** (this slice — mock
  registered-path acceptance) and **4b** (manual/nightly real-credential
  smoke). 4a deliberately drives the same caller-owned boundaries the future
  real smoke should use — bootstrap registration, `ChannelManager::start_all`,
  one explicit `receive_one`, routed prompt dispatch, trace/audit observation,
  and passive QQ send — but replaces the platform with scripted HTTP/WebSocket
  loopback servers so CI can run without secrets or public network. Because it
  does not exercise real QQ credentials, gateway behavior, or platform quotas,
  `channel_qq` remains default-off.
- 2026-06-14 (slice 237): split **4b** into **4b-i** (this slice — the hidden
  opt-in smoke gate) and **4b-ii** (the actual credentialed run result). The
  gate uses the same registered-path boundaries as 4a and keeps the provider
  deterministic with `RecordingProvider`; the only real external dependency is
  QQ itself. The current agent environment has no QQ smoke env vars, so the
  slice can prove the gate compiles, remains hidden by default, and no-ops
  successfully when explicitly selected without credentials, but cannot
  honestly mark the platform smoke complete.
- 2026-06-14 (slice 238): did **not** run 4b-ii because the agent environment
  still has no QQ credentials or operator conversation. Instead, removed one
  avoidable setup requirement for that run: operators no longer need to copy a
  gateway WebSocket URL into `ORAN_TEST_QQ_GATEWAY_URL` because the smoke can
  discover it through the same authenticated QQ API client. Bootstrap config
  still stores an explicit `qq_gateway_url`; production async discovery remains
  a later ownership question because `register_configured_channels(...)` is a
  synchronous boundary.

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
  - `docs/histories/2026-06/20260613-1357-http-websocket-boundary.md`
    (milestone 2b-i, slice 231)
  - `docs/histories/2026-06/20260613-2258-channel-qq-gateway-transport.md`
    (milestone 2b-ii, slice 232)
  - `docs/histories/2026-06/20260613-2321-channel-qq-inbound-adapter.md`
    (milestone 3a, slice 233)
  - `docs/histories/2026-06/20260613-2347-channel-qq-outbound-text.md`
    (milestone 3b, slice 234)
  - `docs/histories/2026-06/20260614-0015-channel-qq-bootstrap-registration.md`
    (milestone 3c, slice 235)
  - `docs/histories/2026-06/20260614-1507-channel-qq-registered-round-trip.md`
    (milestone 4a, slice 236)
  - `docs/histories/2026-06/20260614-1523-channel-qq-real-smoke-gate.md`
    (milestone 4b-i, slice 237)
  - `docs/histories/2026-06/20260614-1551-channel-qq-gateway-discovery.md`
    (milestone 4b-ii prep, slice 238)

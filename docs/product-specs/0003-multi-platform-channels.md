# 0003 — Multi-Platform Channels

## User Problem

Operators want to interact with their Orangutan agent from where they already work:
team chat, mobile messaging, custom HTTP integrations. The legacy `orangutan/` had QQ
only — v2 makes channels pluggable.

## Scope (v1)

- `oran-channel::Channel` trait + `Capabilities` matrix. Shipped in slice 226.
- `oran-channel::ChannelManager` with a bounded inbound fan-in queue. Slice 226
  ships explicit caller-owned `receive_one(...)` fan-in; automatic adapter
  loops and per-conversation strands remain downstream.
- Two adapters shipping:
  - **QQ** (`oran-channel-qq`) — ported from legacy, refactored onto the trait.
  - **Webhook** (`oran-channel-webhook`) — generic inbound HTTP receiver + outbound
    POST.
- xmake options to disable any adapter (`--channel_qq=n`, etc.).
- Per-channel config in `config.channels[]`.

## Scope (v1.1)

- **Discord** (`oran-channel-discord`).
- **Slack** (`oran-channel-slack`).
- **Telegram** (`oran-channel-telegram`).

## Scope (v2 stretch)

- **WebSocket** (`oran-channel-ws`) — generic, bidirectional.
- **Email** (`oran-channel-email`) — read/reply via IMAP/SMTP.
- **Matrix** (`oran-channel-matrix`).

## Out Of Scope

- Voice channels.
- Native mobile app SDKs.

## Acceptance Criteria

1. Compiling with all v1 adapters disabled produces a binary with zero adapter code
   linked in. The foundation `oran-channel` library itself now builds independently
   as of slice 226.
2. A QQ message arrives → routed to the configured agent → response sent back; the
   round-trip is observed via the `audit.db`.
3. A webhook adapter accepts a POST body, the agent processes it, the response is
   POSTed back to the configured callback URL.
4. The capability matrix is honored: an adapter without `reactions` capability never
   crashes when the agent tries to react; the reaction is silently dropped + logged.
5. Per-conversation ordering holds: two messages on the same conversation are
   processed in arrival order; messages on different conversations on the same
   adapter run in parallel.
6. Per-message deadline is enforced: a tool call exceeding the configured deadline
   is cancelled, and the agent emits a follow-up "still working" message.
7. `tests/channel/` ≥ 70% coverage including the webhook adapter end-to-end with a
   mock HTTP server.
8. `bench/channel/` reports inbound throughput ≥ 200 msg/s on the mock adapter.

## Shipped So Far

- Slice 226: `oran-channel` foundation target, public `Channel` trait,
  `Capabilities`, inbound/outbound envelopes, `ChannelManager`, `test-channel`
  coverage, and `bench-channel` direct-append vs. manager fan-in comparison.
- Slice 227: first concrete ingress — in-process `channel::MockChannel`
  (push-driven bounded-queue adapter), the `ChannelPromptRunner` dispatch
  seam with caller-owned `dispatch_one(...)`, and bootstrap's
  `make_channel_agent_prompt_runner(...)` bridging dispatched messages into
  `AgentPromptRunner` with per-conversation session continuity. The
  mock-adapter inbound path benches at ~2.4 µs/message locally (≫ the
  200 msg/s acceptance floor).
- Slice 228: config-authored registration and routing — typed
  `config.channels[]` (`id`, `kind`, `agent_key`, `inbound_capacity`),
  bootstrap `register_configured_channels(...)` into a caller-owned
  `ChannelManager` (unknown kinds skipped + reported), and
  `make_routed_channel_prompt_runner(...)` routing each channel id to its
  configured agent. This closes the v1 "per-channel config in
  `config.channels[]`" scope line for buildable adapters. Webhook/QQ
  adapters and any background receive loop remain open; the QQ port has its
  own plan.
- Slice 229: QQ-port milestone 1 — the gated `oran-channel-qq` library
  (`xmake f --channel_qq=y`, default off until round-trip acceptance) with
  `qq::TokenStore` (single-flight app-access-token refresh, refresh-ahead
  expiry, 401-path `invalidate()`) and `qq::ApiClient` (authenticated
  `get`/`post`/`put`/`del` over `oran-http::Client`, `Authorization: QQBot`,
  the 401/429/gateway retry ladder, `normalize_api_response` trace-id /
  retry-after / business-envelope capture). Request/response shapes
  validated offline against a scripted loopback HTTP server
  (`test-channel-qq` 21 cases / 129 assertions); disabled builds configure
  zero adapter targets, keeping acceptance criterion 1 intact. The receive
  transport, trait adapter, and bootstrap registration follow in the
  QQ-port plan's later milestones.
- Slice 230: QQ-port milestone 2a — the pure `qq::GatewaySession`
  protocol/session state machine: `consume(frame_json)` decodes one gateway
  text frame at a time and returns a `GatewayReaction` (arm heartbeat timer,
  send Identify-vs-Resume, surface a non-lifecycle dispatch, reconnect
  resume/fresh, session-ready) while caching the `s` seq cursor and the READY
  `session_id`; `build_identify`/`build_resume`/`build_heartbeat` emit the
  outbound payloads with the token injected per-call (the session never
  stores the secret, C5); `classify_close_code` maps close codes to a
  recovery action with the SDK-grounded corrections (4009 = the *only*
  resume-able close, 4013/4014 = intents, 4914/4915 = fatal). Offline-tested
  (`test-channel-qq` 40 cases / 209 assertions, 19 new) with no new
  dependency; frames/payloads cross the public surface as JSON strings (C6).
  The `wss://` network transport that drives the session behind
  `Channel::next_message()` is milestone 2b.
- Slice 231: QQ-port milestone 2b-i — the first WebSocket primitive on
  `oran-http` (`http::WebSocket`: `connect`/`receive`/`send_text`/`close` over
  libcurl connect-only mode), the `wss://` transport the gateway session needs.
  It is cancel-aware without a thread (C2/C11): the handshake runs in
  non-blocking `curl_multi_perform` rounds and receive/send suspend on asio
  socket readiness over a `dup` of curl's active socket, so an idle gateway
  connection costs no CPU-pool thread; `close()` completes the RFC 6455 closing
  handshake. The shared libcurl RAII wrappers were extracted to
  `src/oran-http/_impl/curl_common.hpp` (curl stays out of headers, C6).
  `test-http` 27 cases / 148 assertions (12 new WebSocket cases, clean under
  ASan/UBSan), no new dependency. Driving `qq::GatewaySession` over the
  primitive is milestone 2b-ii.
- Slice 232: QQ-port milestone 2b-ii — `qq::GatewayTransport`, the
  caller-owned WebSocket-gateway driver under the `QqChannel` trait adapter.
  It keeps one persistent `http::WebSocket`, drives
  `qq::GatewaySession` lifecycle frames, sends Identify/Resume/heartbeat
  payloads with tokens from `TokenStore`, races receive waits against an
  `async::sleep_for` heartbeat timer, reconnects with the documented
  close-code/backoff policy, invalidates cached tokens on auth close 4004, and
  returns one non-lifecycle `GatewayDispatch` per `next_dispatch()` resume.
  Offline loopback validation covers identify, heartbeat, resume, auth-close
  refresh, token-failure propagation, and cancellation (`test-channel-qq` 45
  cases / 249 assertions). The slice 233 and 234 entries below add
  trait-envelope parsing and passive outbound send; bootstrap registration
  remains milestone 3.
- Slice 233: QQ-port milestone 3a — `qq::QqChannel`, the first trait-adapter
  receive boundary over `GatewayTransport`. `normalize_gateway_dispatch(...)`
  maps C2C and group gateway message events into `channel::InboundMessage`
  (`c2c:{user_openid}` / `group:{group_openid}` conversations, QQ origin,
  reply references, group mention stripping, and text/mention/reply
  capabilities), and `QqChannel::next_message()` skips unsupported non-message
  dispatches until a message arrives. `oran-channel-qq` now depends on
  `oran-channel`; outbound `send(...)` is present on the trait but intentionally
  deferred until the slice-234 text/passive-reply builder. Focused validation:
  `test-channel-qq` 51 cases / 309 assertions.
- Slice 234: QQ-port milestone 3b — outbound passive text replies for
  `qq::QqChannel`. `send(...)` requires an inbound reply target, maps
  `c2c:{user_openid}` / `group:{group_openid}` conversations to
  `POST /v2/users/{openid}/messages` or
  `POST /v2/groups/{group_openid}/messages`, serializes `content`,
  `msg_type:0`, `msg_id`, and `msg_seq`, and parses `id` / `msg_id` /
  `message_id` delivery receipts. The generic `make_reply_message(...)` now
  preserves the first inbound reply reference so the existing dispatch seam can
  produce QQ passive replies. Focused validation: `test-channel` 25 cases / 191
  assertions and gated `test-channel-qq` 55 cases / 344 assertions. At that
  point bootstrap registration still remained off; slice 235 below adds the
  registration seam while full QQ round-trip acceptance stays open.
- Slice 235: QQ-port milestone 3c — gated bootstrap registration for QQ
  channels. `config.channels[]` now accepts QQ-specific env-name / endpoint
  metadata (`qq_app_id_env`, `qq_client_secret_env`, `qq_token_url`,
  `qq_api_base_url`, `qq_gateway_url`) and requires the credential env names
  plus gateway URL when `kind == "qq"`. With `xmake f --channel_qq=y`,
  `register_configured_channels(...)` resolves those env vars, assembles the
  QQ HTTP/token/API/gateway/channel stack through a bootstrap-owned wrapper,
  and registers it into `ChannelManager`; with the default option disabled,
  the same entry is skipped and reported without linking adapter code. Focused
  validation: `test-config` 55 cases / 519 assertions, default
  `test-bootstrap` 146 / 1312, gated `test-bootstrap` 147 / 1319,
  `test-channel` 25 / 191, and gated `test-channel-qq` 55 / 344. Full QQ
  round-trip acceptance remains open.
- Slice 236: QQ-port milestone 4a — mock registered-path round-trip
  acceptance for QQ channels. Under `xmake f --channel_qq=y`, a configured QQ
  channel registers through bootstrap, starts under the caller-owned
  `ChannelManager`, receives one scripted WebSocket gateway C2C message, routes
  through `make_routed_channel_prompt_runner(...)` to the configured agent,
  records a trace turn through the assembly-owned audit/trace repository, and
  sends the passive text reply through a scripted
  `POST /v2/users/{openid}/messages` API call preserving `content`, inbound
  `msg_id`, and `msg_seq`. Focused validation: gated `test-bootstrap` 148 /
  1352. The real-credential manual/nightly smoke gate remains open, so
  acceptance criterion 2 is not yet fully closed and `channel_qq` remains
  default-off.
- Slice 237: QQ-port milestone 4b-i — hidden opt-in real-smoke gate for the
  same registered QQ path. Enabled `test-bootstrap` builds now carry a
  `[.][manual][channel-qq]` case that requires `ORAN_TEST_QQ_REAL_SMOKE=1`,
  QQ credential env vars, and a gateway URL. The smoke keeps the provider
  deterministic, waits for one real operator message, sends the configured
  passive reply through QQ, verifies trace/audit state, and shuts down. This is
  the executable gate for acceptance criterion 2, not the pass result; the
  credentialed run remains open and `channel_qq` remains default-off.
- Slice 238: QQ-port 4b-ii prep — gateway discovery is now a reusable
  `oran-channel-qq` helper. `parse_gateway_bot_response(...)` validates
  `GET /gateway/bot` response bodies into typed gateway URL, shard, and
  session-start-limit values, and `discover_gateway_bot(...)` requests the
  endpoint through the authenticated QQ API client. The hidden real-smoke test
  now treats `ORAN_TEST_QQ_GATEWAY_URL` as an override and discovers the URL
  when it is absent. Focused validation: gated `test-channel-qq` 58 / 369 and
  gated `test-bootstrap` 148 / 1352. The credentialed run itself remains open.

## Design Doc Cross-References

- [`../design-docs/channel-abstraction.md`](../design-docs/channel-abstraction.md)
- [`../design-docs/async-model.md`](../design-docs/async-model.md) (strands, channels)

## Risks

- Platform-specific quirks (Slack thread semantics, Discord rate limits) leak into
  adapter logic — keep adapter-specific docs under `docs/design-docs/channel-<name>.md`.
- Inbound flood from a noisy room saturates the bounded queue — observe via the
  `channel.delivery_error` hook; users can configure a per-conversation rate limit.

## Migration Notes For The QQ Port

- Move `legacy/src/channel/qq/qq-api-client.cpp` to `src/oran-channel-qq/api-client.cpp`,
  but rewrite its libcurl direct use to go through `oran-http::Client`.
- OAuth + token refresh: own state inside `oran-channel-qq::TokenStore`, not in a
  global.
- Attachment cache: lift into `oran-channel::AttachmentCache` (shared by all adapters).
- Group vs. direct-message: keep the legacy distinction; the capability matrix
  expresses it.

## Validation

```sh
xmake f --channel_qq=y --channel_webhook=y --channel_discord=n
xmake build oran-channel oran-channel-qq oran-channel-webhook
xmake test test-channel test-channel-qq
scripts/bench-compare.sh channel
```

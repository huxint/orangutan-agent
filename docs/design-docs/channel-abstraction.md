# Channel Abstraction

The legacy `orangutan/` had ~3.6 kLoC of QQ-specific code with no extracted abstraction.
Adding Discord, Slack, Telegram, or a generic webhook required rewriting all of it. v2
makes channels **a trait with a capability matrix**, and channel adapters become
independent, optional libraries.

## The Channel Trait

```cpp
// include/oran/channel/channel.hpp — PUBLIC
namespace orangutan::channel {

class Channel {
 public:
  virtual ~Channel() = default;

  // Identity for logs, audit, hook events.
  virtual std::string_view id() const noexcept = 0;
  virtual std::string_view kind() const noexcept = 0;  // "qq", "discord", "slack", ...

  // What this adapter can do. The agent UI layer adapts.
  virtual Capabilities capabilities() const noexcept = 0;

  // Lifecycle.
  virtual async::Awaitable<core::Result<void>> start() = 0;
  virtual async::Awaitable<core::Result<void>> stop()  = 0;

  // Inbound: long-poll-style coroutine; one InboundMessage per resume.
  virtual async::Awaitable<core::Result<InboundMessage>> next_message() = 0;

  // Outbound: deliver one message; respect capabilities (silently downgrade
  // unsupported features, e.g. drop reactions on a channel that has none).
  virtual async::Awaitable<core::Result<DeliveryReceipt>>
  send(OutboundMessage) = 0;
};

}  // namespace orangutan::channel
```

The agent runtime is given a `Channel&` (or a `std::shared_ptr<Channel>`) by the
bootstrap. It does not know which adapter is behind it.

Slice 226 ships this foundation in `oran-channel`. Slice 227 adds the first
concrete adapter — the in-process `channel::MockChannel` (external producers
`push_inbound(...)` into a bounded queue; `next_message()` awaits it
long-poll-style; sends are recorded with deterministic receipts) — plus the
channel→agent dispatch seam: `channel::ChannelPromptRunRequest` /
`ChannelPromptRunResult`, the `ChannelPromptRunner` function contract,
`make_prompt_run_request(...)` / `make_reply_message(...)`, and caller-owned
`dispatch_one(manager, runner)` that takes one queued inbound message through
the runner and replies via the owning adapter. `oran-bootstrap` owns the
concrete runner factory (`make_channel_agent_prompt_runner(...)`), which
builds one `AgentPromptRunner` per dispatched message and derives a stable
per-conversation session id — mirroring the automation prompt-runner
precedent so `oran-channel` never depends on agent or bootstrap internals.
Slice 228 adds the config-authored layer on top: the typed
`config.channels[]` block, bootstrap's `register_configured_channels(...)`
(builds buildable adapters into a caller-owned `ChannelManager`, skipping and
reporting unknown or disabled kinds per the adapter-toggle policy below), and
`make_routed_channel_prompt_runner(...)`, which routes each configured channel
id to its `agent_key` bridge. Slice 235 extends that registration seam to QQ
when `--channel_qq=y`: `channels[].kind == "qq"` entries carry QQ credential
env-name / endpoint metadata and bootstrap owns the concrete adapter stack
behind a private `Channel` wrapper before registering it. Slice 236 proves the
registered QQ path under mock gateway/API coverage: one configured QQ message
flows through `ChannelManager`, the routed agent bridge, trace/audit storage,
and the QQ passive-reply API. Real-credential smoke remains the gate before the
adapter can default on. Slice 237 adds the hidden opt-in real-smoke entrypoint
for that gate; slice 238 lets that smoke discover the bot gateway through
`GET /gateway/bot` when no override URL is supplied. The pass/fail
credentialed run remains open; the QQ port is managed by its own exec plan.
Slice 256 adds the first long-lived channel fan-in owner: `orangutan --serve`
now starts buildable `config.channels[]` adapters, runs one pump per adapter
into `ChannelManager`, dispatches fan-in messages through
`dispatch_one(manager, make_routed_channel_prompt_runner(...))`, replies through
the owning adapter, and stops/drains the adapters on shutdown.
Slice 258 lets that same owner act as the first automation trigger producer:
when the automation concern is active, `serve_channels(...)` wraps the routed
runner and enqueues a matching triggered automation event with key
`channel:<channel_id>` before continuing to the direct reply path. Enqueue
failures are reported and do not prevent the channel reply; webhook and
per-conversation scheduling remain downstream.

## Inbound / Outbound Envelopes

```cpp
namespace orangutan::channel {

struct InboundMessage {
  std::string channel_id;          // adapter instance id
  std::string conversation_id;     // jid, channel-id, room-id, …
  std::string user_id;             // platform-native id
  std::string display_name;        // for prompt context
  std::vector<Content> content;    // text + attachments + mentions
  std::vector<Reference> replies_to;  // optional thread / quote context
  core::Time received_at;
  Origin origin;                   // origin::channel::<kind>
  Capabilities caps;               // mirrored from Channel::capabilities()
};

struct OutboundMessage {
  std::string conversation_id;
  std::vector<Content> content;
  std::optional<std::string> reply_to_message_id;
  std::optional<std::string> thread_id;
  std::vector<Reaction> reactions;  // adapter may downgrade
  DeliveryHint hint;                // ephemeral? high-priority? user-mention?
};

struct DeliveryReceipt {
  std::string message_id;          // platform-native; opaque
  core::Time accepted_at;
};

}  // namespace orangutan::channel
```

## Capability Matrix

```cpp
namespace orangutan::channel {

struct Capabilities {
  bool text                 = true;   // baseline; always true
  bool rich_text            = false;  // markdown / formatting
  bool attachments_image    = false;
  bool attachments_file     = false;
  bool attachments_audio    = false;
  bool attachments_video    = false;
  bool reactions            = false;
  bool mentions             = false;
  bool threads              = false;
  bool ephemeral_messages   = false;  // (Slack ephemeral, Discord ephemeral, …)
  bool typing_indicator     = false;
  bool message_edit         = false;
  bool message_delete       = false;
  bool reply_quoting        = false;
  std::size_t max_text_bytes = 4 * 1024;
};

}  // namespace orangutan::channel
```

Adapters fill this honestly at construction. The agent runtime uses it to:

- Decide whether to chunk a long message.
- Decide whether to emit a typing indicator while thinking.
- Decide whether to send `tool_use` previews as ephemeral messages.

## Adapter Library Layout

Each adapter is its own xmake target so users compile only what they need:

```
src/oran-channel/         channel.hpp + ChannelManager
src/oran-channel-qq/      QQ adapter
src/oran-channel-discord/ Discord adapter
src/oran-channel-slack/   Slack adapter
src/oran-channel-telegram/ Telegram adapter
src/oran-channel-webhook/ generic webhook adapter
src/oran-channel-ws/      generic websocket adapter (stretch)
```

xmake option toggles:

```lua
option("channel_qq")       set_default(false)  -- shipped (slice 229); off until round-trip acceptance
option("channel_discord")  set_default(false)
option("channel_slack")    set_default(false)
option("channel_telegram") set_default(false)
option("channel_webhook")  set_default(true)   -- planned
```

An adapter option defaults **on** only after its port's round-trip
acceptance has passed (mock server in CI plus a manual real-credential
smoke test); `channel_qq` therefore ships default-off — see the decision
log in
[`../exec-plans/active/2026-06-10-channel-qq-port.md`](../exec-plans/active/2026-06-10-channel-qq-port.md).

Disabled adapters do not link, do not compile. The `oran-bootstrap` config-loader
silently ignores config entries for disabled adapters with a single warning.

## ChannelManager

`oran-channel::ChannelManager` is the *one* component the agent runtime knows about:

```cpp
namespace orangutan::channel {

class ChannelManager {
 public:
  explicit ChannelManager(asio::any_io_executor, ChannelManagerOptions = {});

  // Register at startup; adapters are owned by the manager.
  core::Result<void> register_adapter(std::unique_ptr<Channel>);

  // Lifecycle.
  async::Awaitable<core::Result<void>> start_all();
  async::Awaitable<core::Result<void>> stop_all();

  // Explicit one-message fan-in; later owners decide how to loop/cancel.
  async::Awaitable<core::Result<void>> receive_one(std::string_view channel_id);

  // Fan-in: returns a bounded async::Channel of InboundMessage from all adapters.
  // The receiver is the agent runtime (or oran-orchestration's dispatcher).
  async::Channel<InboundMessage>& inbound();

  // Direct send (when the agent has a specific channel target).
  async::Awaitable<core::Result<DeliveryReceipt>>
  send(std::string_view channel_id, OutboundMessage);

  // Capability lookup.
  core::Result<Capabilities> caps(std::string_view channel_id) const;
};

}  // namespace orangutan::channel
```

The manager:

- Owns lifetimes of all registered adapters.
- Normalizes one explicit `next_message()` result at a time into a single
  bounded `async::Channel<InboundMessage>`.
- Provides per-channel send.
- Rejects null, unnamed, duplicate, or missing channel ids with `core::Error`
  instead of relying on adapter-specific failure modes.

The manager itself does **not** spawn a background fan-in loop or own
per-conversation serialization. Slice 256 lands the first background fan-in
owner at the actual caller boundary: `bootstrap::serve_channels(...)` under
`orangutan --serve` starts already-registered adapters, runs one pump per
adapter, dispatches from the shared fan-in, can enqueue `channel:<channel_id>`
triggered automation events when `--serve` also owns automation state, and owns
cancellation/shutdown drain. Per-conversation serialization remains downstream.

## Per-Conversation Serialization

Inside one channel, messages for the same `conversation_id` should be handled in order.
The legacy code's `JidTaskRunner` is the right idea; v2 generalizes it via a
`PerKeyStrand<conversation_id>`:

```cpp
// inside ChannelManager
strands_.for_key(msg.conversation_id, [&]() {
  asio::co_spawn(strand_, dispatcher.handle(std::move(msg)), asio::detached);
});
```

Multiple conversations on the same channel adapter run in parallel; a single
conversation's messages are strictly ordered.

Downside of the legacy approach: one slow response blocked the entire JID queue. v2
mitigates by:

- Per-message deadline (`config.channel.<id>.message_deadline_seconds`).
- On deadline, the in-flight tool calls are cancelled; the agent emits a "still
  working" message and rejoins later.

## QQ Adapter Migration

`oran-channel-qq` reuses the legacy QQ API client logic but **decoupled**:

- All curl handling goes through `oran-http::Client`. No direct `CurlHandle` use.
- OAuth + token refresh lives in `oran-channel-qq::TokenStore`, isolated.
- Media handling becomes a separate file under `oran-channel-qq::Attachments` — and
  *the abstraction* (download URL, get bytes, cache) is shared with other adapters via
  `oran-channel::AttachmentCache`.

[`../exec-plans/active/2026-06-10-channel-qq-port.md`](../exec-plans/active/2026-06-10-channel-qq-port.md)
manages the port. Milestone 1 (slice 229) shipped the first two pieces:
`qq::TokenStore` (single-flight app-access-token refresh with refresh-ahead
expiry, `invalidate()` for the 401 path) and `qq::ApiClient` (authenticated
requests with the platform retry ladder — one token refresh on 401, bounded
`retry-after` 429 retries, bounded gateway backoff — plus
`normalize_api_response` for trace-id/retry-after/business-envelope
capture), validated offline against a scripted loopback HTTP server.
Milestone 2a (slice 230) adds `qq::GatewaySession`, the pure gateway
protocol/session state machine: `consume(frame_json)` decodes one gateway
text frame into a `GatewayReaction` (heartbeat-arm, Identify-vs-Resume,
non-lifecycle dispatch, reconnect resume/fresh, session-ready), tracks the
`s` seq cursor and READY `session_id` for Resume continuity,
`build_identify`/`build_resume`/`build_heartbeat` emit outbound payloads
with the token injected per-call, and `classify_close_code` carries the
SDK-grounded close-code corrections (4009 = the only resume-able close;
4013/4014 = intents; 4914/4915 = fatal). It is split deliberately from the
network transport: the protocol is fully offline-testable with no new
dependency, while the `wss://` connection needs a WebSocket primitive the
codebase does not yet have (the legacy adapter used raw `curl_ws_*` on a
dedicated `std::thread`, which C2/C6 forbid). Milestone 2b is itself split:
**2b-i (slice 231)** lands that missing WebSocket primitive on `oran-http`
(`http::WebSocket` over libcurl connect-only mode — cancel-aware without a
thread, the handshake driven by non-blocking `curl_multi_perform` rounds and
receive/send suspending on asio socket readiness; `close()` completes the RFC
6455 closing handshake), with the shared libcurl RAII wrappers extracted to
`src/oran-http/_impl/curl_common.hpp`. **2b-ii (slice 232)** adds
`qq::GatewayTransport`, the caller-owned driver that keeps a persistent
`http::WebSocket`, drives `GatewaySession` lifecycle frames, sends
Identify/Resume/heartbeat payloads with tokens from `TokenStore`, races
receive waits against an `async::sleep_for` heartbeat timer, applies the
documented close-code/reconnect policy, and returns one non-lifecycle
`GatewayDispatch` per `next_dispatch()` resume. **3a (slice 233)** adds the
first `qq::QqChannel` trait-adapter boundary plus the pure
`normalize_gateway_dispatch(...)` seam: C2C and group message dispatches become
`channel::InboundMessage` envelopes with `c2c:{user_openid}` /
`group:{group_openid}` conversation ids, QQ origin, honest text/mention/reply
capabilities, inbound message ids preserved as reply references, and group
mention tokens stripped from prompt text. `QqChannel::next_message()` awaits
`GatewayTransport::next_dispatch()` and skips unsupported non-message dispatches.
**3b (slice 234)** fills the passive text send path: `QqChannel::send(...)`
requires `reply_to_message_id`, maps `c2c:` and `group:` conversations to QQ v2
C2C/group message endpoints, sends text JSON with `content`, `msg_type:0`,
`msg_id`, and process-local `msg_seq`, and parses `id` / `msg_id` /
`message_id` delivery receipts. The generic `make_reply_message(...)` now copies
the first inbound reply reference into `OutboundMessage::reply_to_message_id`, so
the `ChannelManager` → `dispatch_one(...)` path preserves QQ inbound `msg_id`
for passive replies. **3c (slice 235)** wires configured QQ channels into
bootstrap behind `--channel_qq=y`: `oran-config` validates `qq_app_id_env`,
`qq_client_secret_env`, optional `qq_token_url` / `qq_api_base_url`, and
required `qq_gateway_url`; enabled bootstrap builds resolve the env vars at the
credential boundary, assemble `http::Client`, `TokenStore`, `ApiClient`,
`GatewayTransport`, and `QqChannel` in an internal owning wrapper, and register
that wrapper into `ChannelManager`. Default builds still skip/report QQ entries
without linking the adapter. **4a (slice 236)** adds mock registered-path
round-trip acceptance in `test-bootstrap`: bootstrap registration starts the
same QQ stack under `--channel_qq=y`, a scripted WebSocket gateway supplies one
C2C message, the routed prompt bridge runs the configured agent, `audit.db`
receives the trace row, and a scripted QQ API server observes the passive text
reply body with the inbound `msg_id` and `msg_seq`. Real QQ credentials,
platform quotas, and any gateway discovery behavior remain milestone 4b, so
`channel_qq` stays default-off. **4b-i (slice 237)** adds the executable
manual/nightly gate: enabled `test-bootstrap` builds include a hidden
`[.][manual][channel-qq]` case that no-ops successfully unless
`ORAN_TEST_QQ_REAL_SMOKE=1` and the required QQ smoke env vars are present.
When opted in, it registers the configured adapter against real QQ, waits for
one operator message, sends a deterministic fake-provider reply through the QQ
passive API, verifies trace/audit state, and shuts down. The credentialed run
result itself remains 4b-ii before any default-on decision. **4b-ii prep
(slice 238)** adds the typed gateway discovery helper (`GET /gateway/bot`) and
has the smoke use it when `ORAN_TEST_QQ_GATEWAY_URL` is unset, so operators do
not need to copy a transient WebSocket URL into the environment before running
the real smoke.

## New Adapter Recipe

To add a new platform adapter:

1. Open `docs/exec-plans/templates/execution-plan.md`, write a plan.
2. Create `src/oran-channel-<name>/`, `include/oran/channel-<name>/`, `tests/oran-channel-<name>/`,
   `bench/oran-channel-<name>/`.
3. Implement `Channel`, fill `Capabilities` honestly.
4. Add `option("channel_<name>")` in `xmake/options.lua`.
5. Wire into the bootstrap config schema (one new section under `channels:`).
6. Add adapter-specific docs under `docs/design-docs/channel-<name>.md` if the platform
   has noteworthy quirks (e.g. rate limits, OAuth dance).
7. Write at least one integration test that uses a mock HTTP server (`oran-http`'s
   `tests/http/MockServer`).
8. Write a smoke bench: connection setup time, inbound throughput.

## Hook Surface

Channel lifecycle is hookable. See
[`permissions-and-hooks.md`](permissions-and-hooks.md). Events:

- `channel.start`         — adapter started OK.
- `channel.stop`          — adapter stopped (graceful).
- `channel.inbound`       — message received before runtime sees it. Hook can drop the
                            message (rate-limiting), tag it, or rewrite content.
- `channel.outbound.pre`  — before send to platform.
- `channel.outbound.post` — after platform accepts.
- `channel.delivery_error` — send failed; payload includes retry hint.

## Anti-Goals

- **Not** trying to be a generic ESB. Each adapter is goal-built for its platform's
  conversational model.
- **Not** abstracting away platform-specific UX like reactions, ephemeral messages.
  The capability matrix surfaces those rather than pretending all platforms are the same.
- **Not** trying to support websocket adapters as a v1 deliverable; that's stretch.

## See Also

- [`permissions-and-hooks.md`](permissions-and-hooks.md) — every channel checkpoint
  is also a hook point.
- [`../product-specs/0003-multi-platform-channels.md`](../product-specs/0003-multi-platform-channels.md)
  — concrete v1 deliverables.
- Legacy QQ implementation lives in `../../orangutan/src/channel/qq/`. Use as reference,
  not as code to copy.

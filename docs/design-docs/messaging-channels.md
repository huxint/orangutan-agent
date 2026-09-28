# Messaging Channels

`oran-channel` integrates Telegram Bot API, official QQ Bot API and Feishu text
messages. It depends on existing core, async, permission and hook libraries, with
JSON definitions private to its implementation. No messaging SDK or new package
is required. `<oran/channel.hpp>` exposes the host boundary.

## Ports and protocol strategies

`Adapter` is a stateless strategy for authenticated envelope decoding, reply and
activity request construction, capabilities and response validation. The
`telegram()`, `qq()` and `feishu()` factories return process-lifetime strategies.
New protocols implement this same interface. Pure conversion does not open
connections, read configuration, resolve secrets or start agent turns.

`Transport` accepts a conversation and credential-free relative request.
`RunTurn` accepts a normalized message and returns the completed answer. A
`Dispatcher` owns admission, deduplication, delivery progress and activity work.
It borrows its adapter and hook bus and owns its callback values. It has no
provider, session, HTTP, configuration or storage dependency.

Conversation identity includes platform, configured account, direct/group kind,
chat ID and optional thread ID, with length-delimited components. The account
and bot identity come from trusted host configuration. `conversation_key` is a
host session-routing key; map it to one retained `AgentSession` and a durable
session ID. Choose memory scope and sender authority explicitly. A group shares
its conversation; use sender rules if group members have different authority.

| Platform | Accepted ingress | Reply | Activity |
| --- | --- | --- | --- |
| Telegram | `message` text from users in private/group/supergroup chats | `sendMessage`, original message and forum topic | `sendChatAction`, refreshed every four seconds by default; expires naturally |
| QQ | `C2C_MESSAGE_CREATE`, `GROUP_AT_MESSAGE_CREATE` dispatch envelopes | Official `/v2/users` or `/v2/groups` passive text reply | C2C `msg_type: 6`, `input_type: 1`, then explicit `input_type: 2` stop; group activity unsupported |
| Feishu | `im.message.receive_v1` user text events | Message reply with nested JSON content and thread routing | Add `Typing` reaction, then delete the exact returned reaction ID |

Bot/self events where the protocol identifies bots, edits, attachments and other
unsupported event types are ignored; malformed supported events return errors.
Only configured bot mentions are removed, never arbitrary user mentions.
Telegram group mention/command policy belongs to the host admission rules.
Feishu requires the bot open ID to strip its mention placeholders.

Plain text is sent without Telegram Markdown parsing. UTF-8 is validated and
split at code point boundaries using conservative byte limits (Telegram/Feishu
4000, QQ 2000). Empty answers produce no message. QQ replies carry the incoming
`msg_id` and increasing `msg_seq`; direct activity reserves sequences 1 and 2,
so direct reply sequences start at 3, group sequences at 1. Reply preflight
rejects more than five QQ chunks before sending any of them. QQ passive-reply
windows and platform quotas still apply; hosts must reconcile expired replies.
Rich media, cards, editing streamed drafts and QQ guild channels are outside this
text-message contract.

## Hosting and authentication

The application owns ingress servers/polling/gateway connections, signature
verification, replay windows, acknowledgments and durable intake. `decode` is
**not an authentication API**. Call it only after checking the configured account's
Telegram webhook secret header (or receiving authenticated polling results),
QQ callback signature/gateway session, or Feishu signature, verification token
and optional encrypted envelope. Handle webhook verification challenges at that
same host boundary. Acknowledge intake promptly after durable enqueueing; never
wait for an agent turn inside a webhook acknowledgment deadline.

Bootstrap's `channel_http_transport(Client&, ChannelCredential)` binds outbound
requests to the existing HTTP client and fixed official HTTPS endpoints. It uses
a ten-second request timeout and a one-MiB response cap. The credential callback
returns a current Telegram bot token, QQ access token, or Feishu tenant access
token on each request. The host owns environment/secret references and token
refresh (QQ app credentials and Feishu app credentials are not access tokens).
The callback must be cancel-aware and finish borrowed work before returning.
Credentials enter only the HTTP binding, never requests, hooks or diagnostics.
Upstream bodies and credential-callback error text are not propagated as errors.
No channel settings are implicitly read from the runtime configuration.

A host composes these ports on its coordinating strand:

```cpp
channel::DispatcherOptions options;
options.hooks = &hooks;
options.rules = std::move(channel_rules); // allow ChannelReceive/Send/Typing as needed

auto dispatcher = channel::Dispatcher::create(
    channel::telegram(),
    bootstrap::channel_http_transport(http_client, current_credential),
    [&session](channel::Message message) -> async::Awaitable<core::Result<std::string>> {
      auto result = co_await session.run_prompt({.prompt = std::move(message.text)});
      if (!result) co_return std::unexpected(result.error());
      co_return std::move(result->text);
    },
    std::move(options));
```

This example binds one conversation. A multi-conversation host resolves its
session by `conversation_key` inside the turn callback. The host supplies the
HTTP client's worker executor and retains all services until every dispatch has
returned. The bootstrap channel integration test exercises a real `AgentSession`
with a controlled provider and transport.

## Authority, lifecycle and recovery

`ChannelReceive` must be allowed before invoking the turn callback. `ChannelSend`
and `ChannelTyping` require `egress_http` for every request. Rules see normalized
conversation/sender and, for HTTP effects, method/path/body. Unmatched strict
rules and `ask` fail closed. `channel_action` observes the decision before the
effect, exposing only platform, account, operation and allowed status. The
session independently authorizes provider/tool effects through its own policy.

The host calls all dispatcher methods on one strand. Same-conversation overlap
returns `conflict`; total admission and retained events are bounded (default
128, maximum 4096). The host queues and retries admission without dropping the
input. HTTP API failures are checked even when HTTP status is 200; rate-limit
delays from Telegram and numeric `Retry-After` headers are preserved. Sends are
never automatically retried: a timeout can mean the message was delivered.

Activity is advisory and never replaces the agent's result. Starts do not
overlap; renewals stop after two consecutive failures, immediately on rate limit
or permission denial, and at the configured TTL (default/maximum 60 seconds).
QQ direct status and Feishu reactions start once. On completion, failure or
cancellation, the dispatcher stops scheduling, joins any in-flight start and
awaits cleanup. It never releases borrowed services while activity work remains.
Cleanup failures are counted on successful `Delivery` results. Remote outages
can prevent Feishu reaction removal; joined cleanup proves lifetime safety, not
remote success. Telegram typing expires without an explicit stop API.

Delivered events are deduplicated in a bounded FIFO cache. A failed turn is not
rerun on duplicate delivery; a generated answer and confirmed chunk offset stay
in memory after a send failure. `pending` exposes them for host persistence and
reconciliation. `resume` explicitly starts at a host-confirmed unsent offset and
never reruns the agent. `acknowledge_failure` makes a terminal failure evictable
only after the host has handled it durably. Pending failures otherwise apply
backpressure and are never silently evicted. Cache eviction or process restart
ends this in-memory deduplication window: durable intake/outbox and restart
recovery belong to the host, not an exactly-once promise from this library.

## References

The interface split and lifecycle safeguards were informed by OpenClaw's
[channel plugin contract](https://github.com/openclaw/openclaw/blob/main/src/channels/plugins/types.plugin.ts),
[typing controller](https://github.com/openclaw/openclaw/blob/main/src/channels/typing.ts)
and [Feishu typing adapter](https://github.com/openclaw/openclaw/blob/main/extensions/feishu/src/typing.ts).
Wire details follow [Telegram Bot API](https://core.telegram.org/bots/api),
[Tencent's official message types](https://github.com/tencent-connect/botgo/blob/master/dto/message_create.go)
and [Feishu message API](https://open.feishu.cn/document/server-docs/im-v1/message/reply).
These references informed the design; their SDKs and source implementations are
not dependencies. Controlled tests validate the supported wire contract and
lifecycle. Live account permissions and platform acceptance require deployment
validation with the configured bot accounts.

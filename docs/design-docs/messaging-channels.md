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
An optional pure `DispatcherOptions::render_reply` supplies host formatting
before reply preflight and authorization; rules inspect the final request body.
Without it, the dispatcher uses the adapter's ordinary text reply conversion.

Conversation identity includes platform, configured account, direct/group kind,
chat ID and optional thread ID, with length-delimited components. The account
and bot identity come from trusted host configuration. `conversation_key` is a
host session-routing key; map it to one retained `AgentSession` and a durable
session ID. Choose memory scope and sender authority explicitly. A group shares
its conversation; use sender rules if group members have different authority.

| Platform | Accepted ingress | Reply | Activity |
| --- | --- | --- | --- |
| Telegram | User text, photos and image documents with captions in private/group/supergroup chats | `sendMessage`, original message and forum topic | `sendChatAction`, refreshed every four seconds by default; expires naturally |
| QQ | `C2C_MESSAGE_CREATE`, `GROUP_AT_MESSAGE_CREATE` dispatch envelopes | Official `/v2/users` or `/v2/groups` passive text reply | C2C `msg_type: 6`, `input_type: 1`, then explicit `input_type: 2` stop; group activity unsupported |
| Feishu | `im.message.receive_v1` user text events | Message reply with nested JSON content and thread routing | Add `Typing` reaction, then delete the exact returned reaction ID |

Bot/self events where the protocol identifies bots, edits, unsupported attachments
and other unsupported event types are ignored; malformed supported events return errors.
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
requests to the existing HTTP client and fixed official HTTPS endpoints. Supported
Telegram methods cover sends, typing, reactions, ephemeral drafts and file metadata;
arbitrary API paths remain rejected. The binding uses a ten-second request timeout
and a one-MiB response cap. The credential callback
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

This text-only example binds one conversation. Image attachments also need the
host's authorized download and typed prompt mapping. A multi-conversation host
resolves its session by `conversation_key` inside the turn callback. The host supplies the
HTTP client's worker executor and retains all services until every dispatch has
returned. The bootstrap channel integration test exercises a real `AgentSession`
with a controlled provider and transport.

## Telegram deployment host

The opt-in `oran-telegram` executable composes polling, the dispatcher and one
persistent `AgentSession`. It accepts only the explicitly configured user's
private chat. Groups, other senders and unsupported updates are skipped. It
processes one update at a time, keeps Telegram's pending queue intact on startup,
verifies `getMe`, and refuses an active webhook without changing it. Run only one
poller per bot token. The state lock excludes another process using the same
state directory; Telegram rejects competing pollers in different directories.

Photos select the largest available size; image documents keep their Telegram
file ID and caption. After receive admission, `ChannelAttachment` authorizes both
`getFile` and the bounded download from Telegram's fixed file endpoint. The host
accepts JPEG, PNG, GIF and WebP signatures up to 5 MiB, with a 20-second download
timeout. Neither bot tokens nor download URLs enter provider image blocks or
session history. Captions accompany the image; without one, the host asks the
model to inspect it in conversation context. Album entries remain separate turns.
Unsupported or failed downloads receive a visible explanation rather than
silently disappearing. The configured DeepSeek Flash model supports vision
through its [Anthropic-compatible API](https://api-docs.deepseek.com/guides/anthropic_api).

One level of `reply_to_message` retains the original text/caption, message and
sender IDs, and an optional image, including replies to the bot's own messages.
`quote.text` is retained verbatim as the selected passage, independently of the
full original message; the host does not reconstruct it from UTF-16 offsets.
Available external-reply image metadata and quote-only replies also work without
inventing unavailable original text. Nested reply chains are not expanded.
The prompt wraps reply context and the current request in distinct JSON fields
inside the user message. Quoted text remains reference data and never becomes
system instructions. Attached images are numbered: the replied image first,
then the new image, each downloaded through the same permission and size gates.
Text-only messages keep their original prompt representation. This framing follows
the provenance distinction in the [reference prompt](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/system-prompt-project-timeline-user-message-provenance.md).

```sh
xmake build -j4 oran-telegram
# Inject ORAN_TELEGRAM_TOKEN and ORAN_TELEGRAM_MODEL_KEY into the environment.
# Supply the allowed user and host-selected workspace/state directories.
build/linux/x86_64/release/oran-telegram \
  --config apps/telegram/deepseek.example.json --allow-user "$TELEGRAM_USER_ID" \
  --workspace "$WORKSPACE_DIR" --state "$STATE_DIR"
```

`--token-env NAME` selects a different bot-token reference. `--probe --state DIR`
checks identity/webhook status without receiving messages or invoking a model;
it needs only the Telegram credential. `--once` processes one admitted message
and exits. SIGINT/SIGTERM cancels and joins active work. Polls use a 25-second
long-poll timeout, a 35-second HTTP bound and a one-MiB response limit. Retryable
poll failures retry at most four times and respect Telegram rate-limit delays.
Transient server failures are retryable; competing pollers return a terminal
conflict. Transport retry metadata survives error redaction.
Sends and agent turns are never retried automatically by the host.

The DeepSeek example uses the Anthropic-compatible endpoint and the
`ORAN_TELEGRAM_MODEL_KEY` reference, with FileRead, MemoryRecall and MemoryRemember
selected. It permits durable notes and provider requests. Filesystem writes and
delegation are not exposed by this example; other configurations retain normal
session permission decisions. The host provides no interactive approval consumer.
The model receives typed text and image blocks. Local host commands are handled
before any model call or attachment download.

### Chat commands

| Command | Result |
| --- | --- |
| `/new` | Persist a fresh session ID and acknowledge it without calling the model. The next prompt uses a new AgentSession. Existing transcript/checkpoint rows and conversation-scoped long-term notes remain intact. |
| `/status` | Show the active session ID, service uptime, configured model, saved message count, summary coverage and the most recent traced model/token usage for this session. Missing or disabled statistics are explicit. |
| `/help` | Show the supported command catalogue and input guidance. `/start` and `/commands` are aliases; `/start` accepts Telegram's onboarding payload. |
| `/whoami` | Show the admitted Telegram user and chat IDs. |

Commands must be sent alone; unexpected arguments do not execute them. Command
names and this bot's `@username` suffix are case-insensitive. Commands addressed
to another bot are skipped. Only the current text is parsed: quoted text and
image captions never execute commands. Embedded mentions of commands and tokens
containing path separators remain ordinary input. Unknown standalone commands
return help guidance without invoking the model.

After bot/webhook/binding checks, `ChannelMenu` authorizes `setMyCommands` for a
menu scoped to the allowed private chat. Failure is advisory; typed commands
remain available. Probe mode never changes the menu. Command replies use the
same receive/send gates, joined presentation and durable delivery journal as
model replies; commands themselves are not model transcript entries.

`/new` saves the new identity and confirmation together in the pending journal
before sending. An ambiguous confirmation still requires normal reconciliation;
restart never rotates the session again. The cached AgentSession is replaced
at the next model prompt. `/status` requires `ChannelInspect` and reads bounded
metadata and one trace record on the worker executor, without exposing transcript
contents, credential references or filesystem paths. Token figures describe the
last recorded turn, not a live context occupancy estimate.

The host polls serially, so commands wait behind an active turn. This command
surface does not yet interrupt a running turn, change models or force compaction.
Local handling, a native menu and model-free new-session acknowledgments follow
OpenClaw's [slash commands](https://github.com/openclaw/openclaw/blob/main/docs/tools/slash-commands.md)
and [reset handler](https://github.com/openclaw/openclaw/blob/main/src/auto-reply/reply/commands-reset.ts).

### Presentation and persistence

The host renders CommonMark with `cmark`, then sends plain text plus Telegram
native entities. Supported formatting includes emphasis, headings, links, lists,
quotes, inline code and fenced code with language labels. Raw HTML is literal
text; image syntax produces a link, not a media upload. GFM tables, strikethrough
and Telegram custom emoji/sticker payloads are outside this CommonMark surface.
Entity offsets count UTF-16 code units, including emoji. Long answers split at
UTF-8 boundaries and clip/rebase entities per part; a code block retains its
format across messages. Telegram's incompatible code/style overlaps are removed
without dropping text. The journal retains the original Markdown answer.

Provider deltas update an in-memory preview; one joined worker serializes status
and preview requests at one-second intervals. `sendMessageDraft` displays the
latest text page without creating a permanent message. Drafts refresh after
15 seconds while active; Telegram expires them after 30 seconds. Before returning
the answer, the admitted turn stops preview scheduling and joins in-flight draft
requests. Only then may the normal journaled `sendMessage` delivery begin: a late
draft must never reappear after the final message. Reasoning text and tool
arguments never enter previews. Provider retries and tool continuations reset
the preview buffer. Clients that do not show drafts still receive final replies.

After `ChannelReceive` admits the turn, the host uses Telegram-supported reactions:
👀 received, 🤔 thinking, 👨‍💻 tool activity, ✍ answering, 👍 delivered and 😱 failed
or cancelled. Short intermediate states may coalesce. Terminal reactions follow
delivery and remain on the original user message. `ChannelStatus` and
`ChannelDraft` each require `egress_http` and publish a channel hook decision.
These advisory failures do not replace the answer. Two failures disable the
affected feature for that turn, and rate-limit cooldown survives subsequent
turns. Shutdown joins any in-flight feedback before emitting the terminal state
and releasing session resources. Existing typing activity remains independent.

Keep state and credentials outside the agent workspace and its extra filesystem
roots. The host requires a private, locked state directory and rejects a workspace
or extra root containing that directory. It maps configured worker/HTTP bounds,
trace settings, workspace roots and hook timeouts into bootstrap. State binds the
bot ID, allowed user, canonical workspace and stable session ID. Reusing it resumes
conversation history and scoped memory; changing an identity requires a different
state directory. Credentials never appear in journal metadata or console output.

`state.json` records the next update and a pending intake before any turn starts.
Journal JSON is validated at load into typed identity, cursor and pending values;
the on-disk format and saved session identity remain unchanged. Reconciliation
rejects a pending update older than the cursor before writing an archive.
The host saves the generated answer before sending, marks in-flight sends, and
records confirmed chunks before advancing the cursor. An interrupted or failed
turn/send stops the host and leaves the pending record intact. Restart refuses to
replay it, including the crash window between sending and saving a receipt.
The session database retains successful turns independently of delivery.

Inspect the pending update, answer and confirmed chunk count, reconcile any
ambiguous delivery in Telegram, then explicitly retire that exact update:

```sh
build/linux/x86_64/release/oran-telegram --state "$STATE_DIR" --ack-pending "$UPDATE_ID"
```

This archives the complete journal as `handled-<update-id>.json` before advancing the
cursor. It does not rerun the model or resend an answer. Preserve the archive
when manually handling an unsent response. It needs no credentials and refuses
a running host or a mismatched update ID. The host favors recoverable intake and
explicit reconciliation; it does not promise exactly-once remote delivery.

## Dispatcher authority, lifecycle and recovery

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
The Telegram host's coalesced reactions and serialized previews also draw on
OpenClaw's [status controller](https://github.com/openclaw/openclaw/blob/main/src/channels/status-reactions.ts),
[Telegram reaction variants](https://github.com/openclaw/openclaw/blob/main/extensions/telegram/src/status-reaction-variants.ts)
and [preview lifecycle](https://github.com/openclaw/openclaw/blob/main/extensions/telegram/src/draft-stream.ts).
The host uses Telegram's native ephemeral drafts, so preview sends do not need
the persistent-message edit/reconciliation machinery used by that implementation.
Wire details follow [Telegram Bot API](https://core.telegram.org/bots/api),
[Tencent's official message types](https://github.com/tencent-connect/botgo/blob/master/dto/message_create.go)
and [Feishu message API](https://open.feishu.cn/document/server-docs/im-v1/message/reply).
These references informed the design; their SDKs and source implementations are
not dependencies. Controlled tests validate the supported wire contract and
lifecycle. Live account permissions and platform acceptance require deployment
validation with the configured bot accounts.

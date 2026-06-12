# Messaging Platform APIs — Channel-Adapter Reference

Distilled, in-repo reference for the messaging platforms we build (or plan to
build) `channel::Channel` adapters against. Kept here so an agent can implement
an adapter without network access. Keyed to our own
[`Channel` trait + `Capabilities` matrix](../design-docs/channel-abstraction.md),
**not** a verbatim copy of any vendor manual.

> **Scope.** Covers the four platforms researched against current official docs
> and reference implementations on 2026-06-12: **QQ** (current main line),
> **Discord** / **Telegram** (spec-0003 v1.1), and **WeChat** (not currently a
> planned adapter — included for prior-art only; see the WeChat verdict). Slack
> is on the v1.1 roadmap but was not researched in this pass.
>
> **Provenance.** QQ findings are code-grounded against Tencent's own SDKs
> (`tencent-connect/botgo`, `tencent-connect/botpy`) and the
> `tencent-connect/openclaw-qqbot` v1.7.1 channel plugin — these *correct*
> several points the official wiki gets wrong or omits (see "QQ corrections").
> Discord / Telegram / WeChat are doc-confirmed where marked; knowledge-only
> items are flagged inline. Re-confirm anything load-bearing against the live
> source before relying on it.

---

## 1. The transport-model truth table (read this first)

Our trait models inbound as `next_message()` — a one-resume-per-call long-poll
coroutine. That **trait shape is correct for every platform**, but the machinery
*behind* it differs sharply, and "long-poll" is the wrong word for three of the
four:

| Platform | Inbound transport behind `next_message()` | Long-poll? | Needs public HTTPS ingress? |
| --- | --- | --- | --- |
| **QQ** | Persistent **WebSocket gateway** (default) *or* HTTP webhook | **No** | Gateway: no. Webhook: yes (80/443/8080/8443) |
| **Discord** | Persistent **WebSocket Gateway** (only path for ambient messages) | **No** | No (Gateway is an outbound dial) |
| **Telegram** | **`getUpdates` long-poll** *or* `setWebhook` | **Yes** (real) | Long-poll: no. Webhook: yes (443/80/88/8443) |
| **WeChat** | **Push-to-your-server only** (HTTPS callback) | **No** | **Yes, always** |

**Implication for the adapter:** only Telegram fits a literal long-poll loop.
QQ and Discord adapters own a WebSocket read loop (Hello → Identify → heartbeat
timer → Resume on drop) and surface each dispatch frame as one `next_message()`
resume. WeChat has no pull model at all.

> **Docs-in-sync flag.** The QQ-port exec plan, `STATUS.md`, and `ROADMAP.md`
> all describe milestone 2 as a *"long-poll receive transport behind
> `Channel::next_message()`."* That wording is factually wrong for QQ — the
> transport is a WebSocket gateway (or webhook). The trait method stays
> one-resume-per-call; only the description needs correcting. See the QQ-port
> plan's decision log.

---

## 2. QQ — `oran-channel-qq` (current main line)

Tencent **official** bot open platform (`bot.q.qq.com` wiki, API v2). API host
`https://api.sgroup.qq.com`; sandbox `https://sandbox.api.sgroup.qq.com`; token
host `https://bots.qq.com`. The v2 surface targets **group + C2C (单聊)** bots;
the older guild/channel (频道) surface is thinner-capability legacy.

### 2.1 Auth — code-confirmed against our shipped `qq::TokenStore`

- **Endpoint:** `POST https://bots.qq.com/app/getAppAccessToken`, body
  `{"appId", "clientSecret"}` (exact keys). Response `{access_token, expires_in,
  code, message}`.
- **`expires_in` arrives as a JSON *string*** (`"7200"`) in Tencent's own SDKs —
  parse defensively. ✓ Our `parse_expires_in` already handles both string and
  integer (`token_store.cpp:40`).
- **`Authorization: QQBot {access_token}`** on every API call — note the `QQBot `
  prefix, *not* `Bearer` and *not* the deprecated guild-era `Bot {appid}.{token}`.
  ✓ `api_client.cpp:169`. The WebSocket Identify/Resume `token` field uses the
  same `QQBot {token}` string.
- **Single global app-level token.** No per-conversation token. There is a
  documented **60s overlap**: requesting a new token within 60s of expiry
  returns a fresh one while the old stays valid — so refresh-ahead never strands
  in-flight callers. ✓ Our single-flight + `refresh_ahead{300}` is safe.
- Reference SDK refresh timing for parity: `botgo` refreshes at
  `expires_in - 9s` with 0–500ms jitter and panics after >10 consecutive
  failures; the openclaw plugin uses `refreshAhead = 5min`, `minInterval = 60s`.

### 2.2 Inbound — WebSocket gateway (build this first)

`GET /gateway/bot` → `{url, shards, session_start_limit{total, remaining,
reset_after, max_concurrency}}`. Connect `wss://`, then the opcode dance:

| Op | Name | Dir | Notes |
| --- | --- | --- | --- |
| 10 | Hello | recv | first frame; `d.heartbeat_interval` in **ms** — honor it (botgo does; botpy wrongly hardcodes 30s) |
| 2 | Identify | send | `d`: `token` = `"QQBot {tok}"`, `intents` (bitmask), `shard` `[id, n]`, `properties` |
| 6 | Resume | send | `d`: `token`, `session_id` (from READY), `seq` (last `s`) |
| 0 | Dispatch | recv | carries `s` (cache it), `t` (event type), `d` |
| 1 | Heartbeat | send/recv | payload `d` = last `s` (may be `null` before first dispatch) |
| 11 | Heartbeat ACK | recv | — |
| 7 | Reconnect | recv | reconnect + **Resume** (session preserved) |
| 9 | Invalid Session | recv | `d` is a **bare boolean**, not an object; `false` → drop session, fresh Identify |
| 12 / 13 | HTTP Callback ACK / URL validation | — | webhook transport only |

**Resume vs fresh Identify (from openclaw `gateway.ts`):** Resume iff
`session_id && last_seq != null`, else Identify. Persist `session_id` + `seq` to
survive restarts (Resume then replays missed events).

**Close-code table — code-grounded from `botgo/errs/err.go` (the wiki page 404s):**

| Code | Meaning | Recovery |
| --- | --- | --- |
| 4004 | auth failed | **refresh token**, reconnect |
| 4006 | session no longer valid | drop session, fresh Identify |
| 4007 | invalid seq | drop session, fresh Identify |
| 4008 | rate limited | reconnect after ~60s |
| 4009 | session timeout | **resume-able** — the *one* unexpected close that permits Resume |
| 4010–4014 | bad shard / sharding required / bad api version / **invalid intents (4013)** / **unauthorized intents (4014)** | fix config, fresh connect |
| 4914 | **bot offline / delisted** | **fatal — no reconnect** |
| 4915 | **bot banned** | **fatal — no reconnect** |

> **Correction:** the doc-only research pass claimed "4914 = invalid intents,
> 4915 = intents unauthorized." That is **wrong** per botgo source. Invalid /
> unauthorized intents are **4013 / 4014**; 4914 / 4915 are bot-lifecycle
> (offline / banned) and are fatal. Everything unexpected *except 4009* →
> cannot-resume → fresh Identify.

openclaw backoff for parity: `RECONNECT_DELAYS = [1s,2s,5s,10s,3s,60s]` indexed
by attempt (clamped); 3 disconnects within 5s each → wait 60s (likely
credential/permission problem); `MAX_RECONNECT_ATTEMPTS = 100`.

**Intents bitmask** (1<<iota; from botgo/botpy/openclaw — all agree on the
load-bearing bits):

- `GROUP_AND_C2C = 1<<25` (= 33554432) — **the single bit that gates both group
  AND C2C private messages.** This is *the* bit a group/C2C bot needs.
- `INTERACTION = 1<<26` (= 67108864) — button/inline-keyboard callbacks.
- `GUILDS = 1<<0`, `GUILD_MEMBERS = 1<<1`, `PUBLIC_GUILD_MESSAGES = 1<<30`,
  `DIRECT_MESSAGE = 1<<12`, `MESSAGE_AUDIT = 1<<27`, `FORUMS = 1<<28`.
- **An unauthorized intent closes the WS (4014).** Default-grantable: GUILDS,
  PUBLIC_GUILD_MESSAGES, GUILD_MEMBERS; the rest need console approval.
- Naming diverges between SDKs for bits 9 and 30 (botgo `GuildAtMessage` vs
  botpy `public_guild_messages`) — pick one mapping and document it. The openclaw
  plugin sends a fixed `FULL_INTENTS = 1<<30 | 1<<12 | 1<<25 | 1<<26 =
  1174310912` with no per-bot subsetting.

### 2.3 Inbound — HTTP webhook (alternative)

Tencent is steering newer group/C2C bots toward webhook, so the receive
transport should sit behind a small swappable interface (gateway *or* webhook),
which NoneBot's official `adapter-qq` proves must coexist long-term.

- HTTPS callback URL, ports **80/443/8080/8443** only.
- **op-13 URL-validation handshake** (handled *before* signature verification,
  since it carries no signature): inbound `{op:13, d:{plain_token, event_ts}}` →
  reply `{plain_token, signature}` where `signature = hex(ed25519_sign(seed,
  event_ts + plain_token))`.
- **op-12 ACK:** after verifying an event push, respond synchronously `200`
  `{op:12, d:0}`, *then* dispatch the event fire-and-forget.
- **Ed25519 seed derivation:** repeat the bot **secret** until ≥32 bytes, then
  truncate to 32 bytes (UTF-8; ASCII secrets are the normal case so byte-naive
  truncation is fine). Inbound event signature verifies `timestamp + body`
  (timestamp first); headers `x-signature-ed25519` (hex) + `x-signature-timestamp`.
- Webhook needs an Ed25519 dependency; the gateway does not — a point in the
  gateway's favor given `rules/libraries.md`'s "no new third-party dep expected."

### 2.4 Outbound

| Scene | Endpoint |
| --- | --- |
| C2C single | `POST /v2/users/{openid}/messages` |
| Group | `POST /v2/groups/{group_openid}/messages` |
| Guild channel | `POST /channels/{channel_id}/messages` (v1 shape — no `msg_type`/`msg_seq`) |
| Guild DM | `POST /dms/{guild_id}/messages` |
| C2C/group media upload | `POST /v2/{users\|groups}/{id}/files` |
| C2C/group recall | `DELETE /v2/{users\|groups}/{id}/messages/{message_id}` |

**v2 group/C2C body:** `content`, `msg_type` (0 text, 2 markdown, 3 ark, 4 embed,
7 media), `msg_id` (reply target — present ⇒ passive; absent ⇒ proactive),
`msg_seq` (dedup paired with `msg_id`; same `(msg_id, msg_seq)` repeat **fails**),
optional `media{file_info}`, `markdown`, `keyboard`, `ark`, `message_reference`,
`event_id`. (`msg_type` enum differs slightly between botgo `{0,2,3,4,5,6,7}` and
botpy `{0,1,2,3,4,7}` — follow botpy for v2 group/C2C.)

**Passive-reply window is the dominant constraint:**

- Reply must carry the inbound `msg_id` (or `event_id` for event-triggered
  replies). **Active push was discontinued 2025-04-21** — treat the adapter as
  **passive-reply-only**.
- Quota: **C2C ≤5 replies within 60 min** per `msg_id`; **group ≤5 within 5 min**;
  guild channel/DM 5-min window. The openclaw plugin conservatively tracks **4
  replies / 1h** client-side and **silently downgrades to a proactive (quota-
  limited) message** when exhausted. The runtime must treat an agent's reply as a
  bounded set of passive chunks and may need to merge if it produces >5.
- `msg_seq`: openclaw generates it *statelessly* as `(ms_timestamp ⊗ random) %
  65536` rather than a per-`msg_id` counter, tolerating collisions over
  maintaining state.
- `message_reference` (quote) works **only in plain-text mode**, not markdown.

**Media:** two-step. Upload to `/files` (`file_type`: 1 image, 2 video, 3 voice
silk, 4 file — **file type 4 not open for groups**) → returns `{file_uid,
file_info, ttl}` → send `msg_type:7` with `media.file_info`. `ttl=0` means the
`file_info` is long-term reusable (Telegram `file_id`-style); cache it. Large
local files use a 3-step chunked `upload_prepare` → presigned-PUT →
`upload_part_finish` flow. Embedded URLs in message content must be pre-registered
in the console or the send fails; openclaw "defangs" dots (`a.b`→`a_b`) in group
plain-text to dodge URL rejection.

### 2.5 Capabilities — **conversation-kind-dependent, not one flat struct**

This is the key modeling note: QQ's capability surface differs by conversation
kind. Our `Capabilities` is returned per-channel today; a QQ adapter instance
may need to vary it by inbound kind (or expose the union and downgrade honestly).

| `Capabilities` field | Group / C2C | Guild (频道) |
| --- | --- | --- |
| `text` | ✓ | ✓ |
| `rich_text` | ✓ markdown (`msg_type:2`, **approval-gated** template) | ✓ |
| `attachments_image/audio/video` | ✓ via 2-step upload (`msg_type:7`) | image as markdown `![](url)` only; voice/video refused |
| `attachments_file` | ✗ (`file_type:4` not open for groups) | — |
| `reactions` | **✗ — guild-only API** | ✓ `PUT/DELETE /channels/{c}/messages/{m}/reactions/...` |
| `mentions` | ✓ (group triggers on `GROUP_AT_MESSAGE_CREATE`, @-only) | ✓ |
| `threads` | ✗ | ✓ forums (`1<<28`, private-domain, gated) |
| `ephemeral_messages` | ✗ | ✗ |
| `typing_indicator` | partial — `msg_type:6` input-notify (C2C) | ✗ |
| `message_edit` | ✗ (no edit endpoint) | ✗ |
| `message_delete` | recall **✗ on group/C2C** in practice | ✓ `DELETE /channels/{c}/messages/{m}` |
| `reply_quoting` | ✓ `message_reference` (plain-text only) | ✓ |
| `max_text_bytes` | not documented; openclaw chunks at 5000 chars | not documented |

C2C also supports **streaming replace** (`POST /v2/users/{openid}/stream_messages`,
`input_state` 1=generating/10=done) — no group streaming.

### 2.6 Conversation-id mapping

- C2C → `c2c:{user_openid}`; reply `POST /v2/users/{user_openid}/messages`.
- group → `group:{group_openid}` (or `:{member_openid}` to key per-speaker);
  reply `POST /v2/groups/{group_openid}/messages`.
- guild → `guild:{channel_id}` (carry `guild_id` for tenancy); reply
  `POST /channels/{channel_id}/messages`.

`openid` is **per-bot-per-user**, not portable across apps. `union_openid` exists
at app-identity level but is **not** in the message receive payload — do not route
on it. Stash inbound `msg_id` + a per-`msg_id` reply counter to honor the 5-reply
window.

### 2.7 QQ corrections (doc-only research → SDK truth)

1. **No long-poll** — WebSocket gateway or webhook (see §1).
2. **4914/4915 = offline/banned (fatal)**, not invalid intents (those are 4013/4014).
3. **4009 is the only resume-able unexpected close.**
4. `expires_in` is a string; `Authorization: QQBot ` not `Bearer`; trace header
   `X-Tps-trace-id` (case varies between SDKs — match case-insensitively, which
   our `iequals` already does).
5. **`tencent-connect/bot-oas` is v1-only** (guild/channel/DM) — no v2 group/C2C,
   no media-upload, no token endpoint, no error schema. Use `botgo`/`botpy` source
   as ground truth for v2.

### 2.8 Reference implementations (official / quasi-official)

- `tencent-connect/botgo` (Go) — closest to our model (typed payloads, explicit
  close-code handling). **Best ground truth** for endpoints, opcodes, intents.
- `tencent-connect/botpy` (Python) — confirms payload field names + ID mapping.
- `tencent-connect/openclaw-qqbot` v1.7.1 (TS) — a full AI-assistant **channel
  plugin** (the user's named reference); confirms both transports, the
  passive-reply tracker, media pipeline, and capability declarations.
- `nonebot-adapter-qq` (official NoneBot2) — dual WS+webhook reference.
- **Avoid:** OneBot / go-cqhttp ecosystems — those drive *unofficial personal-
  account* automation (ToS/ban risk), a different thing from the open platform.

---

## 3. Discord — `oran-channel-discord` (spec-0003 v1.1)

Current stable API **v10**; docs at `docs.discord.com/developers`. Bot token
`Authorization: Bot <token>` — **static, no refresh** (regenerate via portal).
OAuth2 bearer is only for acting on a user's behalf; a chat bot doesn't need it.

- **Inbound: Gateway WebSocket only** for ambient messages. The HTTP Interactions
  webhook delivers *only* slash-command/component interactions, never
  `MESSAGE_CREATE`. Same opcode model as QQ (Hello/Identify/Heartbeat/Resume),
  resume via `resume_gateway_url` (**not** the original connect URL — common bug).
- **Privileged `MESSAGE_CONTENT` intent (`1<<15`)** is the biggest gotcha: without
  it, inbound `content`/`embeds`/`attachments` are empty except in DMs / messages
  that mention the bot. Must be enabled in the portal (+ approved once verified).
  Intents ≠ permissions ≠ OAuth scopes — three separate systems.
- **Outbound:** `POST /channels/{id}/messages`; **2000-char hard cap for bots**
  (Nitro's 4000 is a human-UI feature, no API bypass — chunk or use embeds, 6000
  total). DMs require opening a DM channel first (`POST /users/@me/channels`).
- **Rate limits:** per-route buckets keyed by top-level resource; read
  `X-RateLimit-Bucket`/`-Remaining`/`-Reset-After`, honor 429 `retry_after`
  (float seconds). Don't hardcode limits. >10k invalid responses / 10min →
  Cloudflare IP ban.
- **`conversation_id` = `channel_id`** (uniquely identifies a guild text channel,
  thread, *or* DM — exactly what `POST .../messages` targets); carry `guild_id`
  as optional context (null ⇒ DM). Threads are themselves channel ids. **Treat
  Snowflake IDs as strings.**

Capability highlights vs our matrix: `rich_text` ✓ (Discord markdown),
`reactions` ✓, `threads` ✓, `mentions` ✓ (control via `allowed_mentions` — defaults
differ by context, set explicitly to avoid mass-pings), `typing_indicator` ✓
(`POST /channels/{id}/typing`, self-expires ~10s — re-send for long turns),
`message_edit`/`message_delete` ✓, `reply_quoting` ✓ (`message_reference`),
`ephemeral_messages` ✓ **but interaction responses only** (`flags:64`), not plain
sends. `max_text_bytes`: 2000 chars.

Framework pattern: the library owns the Gateway loop and exposes `on_message`
callbacks (discord.py, discord.js). Rust's **twilight** splits gateway/http/
cache/model crates — a useful architectural reference for a trait-based adapter.

---

## 4. Telegram — `oran-channel-telegram` (spec-0003 v1.1)

Bot API over HTTPS: `https://api.telegram.org/bot<token>/METHOD`. Token from
BotFather — **static, no refresh** (revoke/regenerate only). The **only** platform
here with a real long-poll.

- **Inbound (`getUpdates` — clean fit for `next_message()`):** params `offset`,
  `limit` (1–100), `timeout` (set 30–50s for true long-poll; default 0 = short
  poll), `allowed_updates`. **The offset IS the ack:** an update is confirmed once
  you call `getUpdates` with `offset > its update_id`; confirmed updates are
  dropped. Persist `max(update_id)+1` to dedup across restarts. **Single-flight
  per bot** — overlapping polls → `409 Conflict`. `getUpdates` and `setWebhook`
  are **mutually exclusive**.
- `allowed_updates` empty/omitted excludes `chat_member`, `message_reaction`,
  `message_reaction_count` — explicitly list `"message_reaction"` to get reactions.
- **Outbound `sendMessage`:** `chat_id` + `text` (**≤4096 chars**, characters not
  bytes — split on entity-safe boundaries). `parse_mode` `MarkdownV2`/`HTML`/legacy
  `Markdown`, or explicit `entities`. Reply+quote via `reply_parameters`
  (`message_id`, `quote` 0–1024 chars, etc.).
- **`MarkdownV2` escaping is strict** — reserved chars ``_ * [ ] ( ) ~ ` > # + -
  = | { } . !`` must be backslash-escaped or the whole send 400s. **Prefer `HTML`
  parse_mode for generated/LLM content** (only `<`, `>`, `&` need escaping).
- **Rate limits:** ~1 msg/s per chat, ≤20/min per group, ~30/s global. 429 body
  carries `parameters.retry_after` (seconds) — honor it exactly. (MTProto layer
  uses `FLOOD_WAIT` / 420 — don't confuse with Bot API 429.)
- **File limits:** download ≤20 MB, upload ≤50 MB (self-hosted Bot API server
  raises both).
- **`conversation_id` = `chat_id`** (positive = private = user id; negative =
  group/channel, supergroups `-100`-prefixed); compose `chat_id:message_thread_id`
  for forum topics. `from.id` = author, `message_id` = reply/edit target.

Capability highlights: `rich_text` ✓, attachments ✓ (per-type send methods),
`reactions` ✓ (`setMessageReaction`, opt-in update), `mentions` ✓, `threads` ✓
(forum topics via `message_thread_id`), `typing_indicator` ✓ (`sendChatAction`,
~5s), `message_edit`/`message_delete` ✓, `reply_quoting` ✓ (substring quote),
`ephemeral_messages` ✗. `max_text_bytes`: 4096 chars.

Framework pattern: one dispatcher, two ingress modes — `Updater` owns the fetch
loop and feeds a queue the `Application`/`Dispatcher` consumes
(python-telegram-bot, aiogram, Telegraf). Mirror this: a `TelegramChannel` owns a
single long-poll loop behind `next_message()`, normalizes each `Update`, and
sends over `sendMessage` with HTML parse_mode + 4096 splitting + `retry_after`
backoff.

---

## 5. WeChat — prior-art only (not a planned adapter)

**Honest verdict:** a ToS-compliant WeChat adapter is meaningfully harder than the
others and is **not currently on our roadmap**. Recorded here so a future
decision is informed, not to imply intent.

- **Personal WeChat has no official API.** Most "wechat bot" repos (and Wechaty's
  default puppets) drive unofficial personal-account automation — **ToS violation,
  ban risk. Do not build on it.**
- **Easiest compliant target: WeChat Work (WeCom) group webhook bot** — outbound-
  only `POST` to a per-robot URL, no token, no callback, no crypto (20 msgs/min).
  Phase 2 would be a WeCom self-built app (bidirectional, enterprise-scoped,
  official C++ crypto lib available).
- **Consumer Official Account (Service Account)** is the real consumer-bot path but
  is the expensive tier: enterprise verification + ICP filing, a **global single
  access_token** (use the `stable_token` endpoint to avoid mutual invalidation),
  **AES-256-CBC + XML** wire encryption, and a **hard 5-second callback deadline**
  that forces async: ACK the push immediately, then push the real answer via the
  customer-service API — which binds you to a **48h / 5-message window**.
- Capability surface is thin: no reactions/threads/edit/quote on 1:1; rich text is
  card-based (`news`/`mpnews`), not general markdown. `aimsgcontext.is_ai_msg` adds
  a gray "AI-generated" tag — relevant for an agent.
- `conversation_id` = `(account_id, openid)`; `openid` is per-app, `unionid` stable
  across one Open Platform account.

Compliant reference libs: WeChatPy / WeRoBot (Python), Tencent's official WeCom
encrypt/decrypt SDKs (incl. C++). **Avoid** modeling on Wechaty's default behavior.

---

## 6. Cross-cutting notes for the `Channel` trait

- **`next_message()` is the right shape everywhere**, but back it with a swappable
  *receive transport* for QQ (gateway ↔ webhook) and Telegram (poll ↔ webhook).
  Only Telegram is a literal long-poll.
- **Build the WebSocket gateway before the webhook** for QQ/Discord: it's an
  outbound dial (no public ingress, no inbound auth surface, no Ed25519 dep) and
  maps cleanly to one persistent coroutine + heartbeat timer reusing
  `async::sleep_for`. Webhook is the right second transport for headless/no-egress
  deployments. **A webhook endpoint is an unauthenticated public surface until its
  signature verification is correct** — treat it as security-sensitive.
- **Capabilities may need to vary by conversation kind** (QQ group/C2C vs guild).
  Fill the matrix honestly; the agent layer downgrades (e.g. drops reactions where
  unsupported) per the design doc's anti-goal of not pretending platforms are
  uniform.
- **Per-conversation passive-reply windows + quotas** (QQ: 5 replies / 5–60 min)
  belong in the adapter, not the runtime. Track inbound `msg_id` → reply count;
  decide merge-vs-drop when an agent turn exceeds the quota.
- **Text caps differ wildly** (Discord 2000, Telegram 4096 chars, QQ ~5000 by
  convention) — set `max_text_bytes` per adapter and chunk on entity-safe
  boundaries (markdown fences, mention spans).
- **IDs are strings** (Discord Snowflakes, QQ openids). Don't coerce to integers.

---

## Sources

QQ (code-grounded): `tencent-connect/botgo`, `tencent-connect/botpy`,
`tencent-connect/openclaw-qqbot` v1.7.1, `nonebot/adapter-qq`; wiki
`bot.q.qq.com/wiki/develop/api-v2/`.
Discord: `docs.discord.com/developers` (gateway, opcodes, rate-limits, oauth2,
interactions, message/channel resources).
Telegram: `core.telegram.org/bots/api`, `/bots/faq`, `/api/entities`;
python-telegram-bot architecture wiki.
WeChat: `developers.weixin.qq.com/doc/service/`,
`developer.work.weixin.qq.com/document/`.

Confidence: QQ items are code-confirmed unless noted; Discord/Telegram/WeChat are
doc-confirmed where the per-platform sections do not flag otherwise. Reconfirm
load-bearing details against live source before coding — vendor docs (esp. the QQ
and WeChat wikis) drift and reorganize.

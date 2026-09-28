# Messaging channels

## Objective

Integrate official QQ Bot API, Telegram Bot API and Feishu through a common,
SDK-free text-message boundary and a joined turn lifecycle.

## Contracts and slice

Keep wire parsing/request construction independent of sessions, credentials and
HTTP execution. Add an oran-channel library with normalized inbound messages,
platform capabilities and a bounded conversation dispatcher. Inject agent turns
and HTTP transport; bootstrap binds HTTP and refreshed credential lookup.
The host authenticates webhook/gateway envelopes before decoding and owns ingress
acknowledgments and durable delivery queues. Support Telegram private/group/topic
text, QQ C2C/group mention text, and Feishu user text messages. Reject malformed
messages, ignore unsupported events, preserve reply routing and isolate accounts.

Typing is advisory: renew Telegram chat actions while running, add/remove a
Feishu Typing reaction, and make QQ's absent typing capability explicit. Join
status work and cleanup on success, failure and cancellation. Retain generated
answers across delivery failures, suppress duplicate events in a bounded cache,
and reject overlapping turns for the same conversation. Do not automatically
retry ambiguous sends.

## Verification and completion

Test wire fixtures, bounds, routing, Unicode splitting, API errors, permission
denial, duplicate delivery, failed sends, status cleanup and cancellation using
controlled effects. Run release builds/tests, ASan/UBSan for new lifetime work,
and make ci. Document host ingress/authentication and delivery responsibilities,
update architecture/build inventory, inspect changes and commit. No platform
credentials are available; live platform acceptance remains deployment validation.

## Progress

- Repository contracts inspected; official QQ selected by the user.

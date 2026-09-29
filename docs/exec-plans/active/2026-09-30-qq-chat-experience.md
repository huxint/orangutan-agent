# QQ conversation experience

## Objective

Extend the verified QQ loop with referenced-message context, images, native
Markdown, joined input status and clear local commands. Use Tencent's official
QQ plugin/SDK and the existing Telegram host as concrete product references.

## Contracts and implementation

- Channel values preserve QQ's own message/reference indices and image attachment
  metadata. Resolve references only from the same admitted conversation's saved
  incoming messages and confirmed outgoing receipts, never guess missing text.
- Download supported QQ CDN images only after sender admission and an explicit
  attachment permission/hook. Bound bytes/time, validate image signatures, and
  pass typed image content to AgentSession. Explain unavailable/unsupported images.
- A pure QQ Markdown renderer supplies native type-2 messages through dispatcher
  preflight. Keep passive-reply sequencing and the five-part limit. Input status
  uses the existing joined activity lifecycle and cannot fail the user answer.
- Local /help, /status and /new run without a model, use the same durable delivery
  journal and preserve transcripts. A new session keeps conversation memory.
- Extend the bounded live evaluator and reproducible Gateway test entry point;
  do not claim a production multi-account host or unsupported media generation.

## Verification

Channel tests cover real SDK envelope shapes, references, image-only input,
malformed metadata and native Markdown. Injected attachment tests cover denied
network, URL/byte bounds, signature checks and quoted prompt mapping. Evaluator
regressions cover commands, preserved state and restart. Run affected release
and sanitizer tests and make ci. Confirm quoting, images, formatting and status
in real QQ when the account/model permits them; report concrete limits.

## Completion

Update messaging/build/testing contract owners, remove this plan, commit and
push the complete verified slice. Preserve all live journals and user records.

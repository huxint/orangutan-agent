# Channel Boundaries And QQ Webhook Integration

## Objective

Remove Telegram command/storage coupling, narrow outbound channel authority, and
implement the smallest official QQ ingress/credential slice on existing runtime
ports. Preserve all deployed Telegram records and behavior.

## Findings And Scope

- Telegram command declarations include the full host/JSON/filesystem surface;
  status rendering directly queries storage and hardcodes the agent key. Move
  scoped status reads to bootstrap values and keep application formatting pure.
- QQ and Feishu HTTP allowlists currently accept entire API prefixes and both
  HTTP methods. Limit them to supported message/reaction operations before
  credential lookup.
- QQ currently has only trusted-envelope decoding and reply construction. Its
  official SDK uses signed Webhook callbacks; do not build a deprecated WebSocket
  gateway or treat decoding as authentication.
- Add bounded Ed25519 verification, timestamp freshness, challenge/heartbeat
  handling and a durable-enqueue port whose success precedes dispatch ACKs.
- Add an account-scoped, cancel-aware QQ token source with injected bounded HTTP,
  permission/hook decisions, expiry-aware reuse and serialized refresh. Never
  log token response bodies or app secrets.
- Verify authenticated enqueue then AgentSession/Dispatcher delivery using
  controlled HTTP and a persistent session. The embedding host owns public TLS,
  durable queue/outbox and workers; this slice does not claim a deployed QQ bot.

## Verification

Use Tencent BotGo's signature test vector, tamper/stale/oversize cases, failed
enqueue ACKs, denied effects, concurrent/cancelled refresh, secret redaction,
route restrictions and reopened-session continuation. Run affected builds, all
release tests, relevant ASan/UBSan checks and `make ci`. Update the owning
contracts and deployment prerequisites, remove this plan and commit verified work.

## Source Contracts

- [Tencent SDK overview](https://github.com/tencent-connect/botgo/blob/master/README.md)
- [Webhook protocol](https://github.com/tencent-connect/botgo/blob/master/interaction/webhook/webhook.go)
- [Signature algorithm](https://github.com/tencent-connect/botgo/blob/master/interaction/signature/interaction.go)
- [Token source](https://github.com/tencent-connect/botgo/blob/master/token/token_source.go)

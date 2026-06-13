## [2026-06-13 23:47] | Task: QQ Outbound Text Replies

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: Codex CLI/API workspace
- Linked plan: `docs/exec-plans/active/2026-06-10-channel-qq-port.md`

### User Query

> Continue the long-running slice workflow with an effectively unbounded goal
> budget: read the development docs and true current progress first, choose the
> highest-value current slice, keep docs in sync, validate, and commit with a
> proper detailed message.

### Changes Overview

- Areas: `oran-channel`, `oran-channel-qq`, QQ-port milestone 3b.
- Key actions: implemented `qq::QqChannel::send(...)` for passive text replies
  over `ApiClient`; mapped `c2c:` and `group:` conversation ids to QQ v2
  message endpoints; serialized `content`, `msg_type:0`, `msg_id`, and
  process-local `msg_seq`; parsed `id` / `msg_id` / `message_id` receipts;
  rejected unsupported outbound shapes before HTTP; propagated inbound reply
  references through generic `channel::make_reply_message(...)`; bumped the
  binary slice tag to `2.0.0-slice234`.

### Design Intent

Slice 233 validated the receive-side trait adapter and deliberately left
outbound QQ semantics isolated. This slice completes the smallest send path that
can participate in the existing `ChannelManager` → `dispatch_one(...)` seam:
only passive text replies are supported because QQ v2 active push is documented
as discontinued, so a missing `reply_to_message_id` is a local
`invalid_argument` instead of an attempted proactive send. `msg_seq` is a
process-local monotonic value for the first adapter slice; a durable per-`msg_id`
quota/chunk tracker belongs with round-trip acceptance after bootstrap can
assemble configured QQ channels.

### Files Modified

- `include/oran/channel-qq/channel.hpp`
- `include/oran/channel/dispatch.hpp`
- `src/oran-channel-qq/channel.cpp`
- `src/oran-channel/dispatch.cpp`
- `tests/channel-qq/test_channel.cpp`
- `tests/channel/test_dispatch.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped slice/current history, refreshed `oran-channel` and
  `oran-channel-qq` test counts, and set the next intended QQ slice to bootstrap
  registration.
- `docs/ROADMAP.md` — moved the Channels frontier to slice 234 and recorded the
  next registration step.
- `docs/exec-plans/active/2026-06-10-channel-qq-port.md` — marked milestone 3b
  complete and logged the passive-text-only decision.
- `docs/design-docs/channel-abstraction.md` — documented the shipped passive send
  path and generic reply-reference propagation.
- `docs/product-specs/0003-multi-platform-channels.md` — added the slice-234 QQ
  shipped-so-far entry.
- `docs/ARCHITECTURE.md` — refreshed the `oran-channel-qq` inventory row.
- `docs/BUILD_SYSTEM.md` — documented the gated adapter target's current
  receive/send state without adding dependencies.
- `docs/QUALITY_SCORE.md` — refreshed test counts and Channels status.
- `docs/releases/feature-release-notes.md` — added the user-visible slice note.

### Validation

- Commands run:
  - `xmake f --channel_qq=y`
  - `xmake build test-channel`
  - `xmake build test-channel-qq`
  - `xmake run test-channel`
  - `xmake run test-channel-qq`
  - `xmake build bench-channel-qq && xmake run bench-channel-qq`
  - `xmake build bench-channel && xmake run bench-channel`
  - `xmake f --channel_qq=n && xmake build orangutan && xmake run orangutan -- --help`
- Tests added/changed:
  - `tests/channel/test_dispatch.cpp` proves `make_reply_message(...)` carries
    the first inbound reply reference into outbound replies.
  - `tests/channel-qq/test_channel.cpp` covers C2C passive sends, group passive
    sends, local rejection of unsupported outbound shapes, and malformed receipt
    parsing.
- Bench impact: no new bench scenario; existing `bench-channel` and
  `bench-channel-qq` buckets still build and run.
- Compile-budget delta: one small change in the gated `oran-channel-qq` TU plus
  a small `oran-channel` dispatch edit; default `--channel_qq=n` builds compile
  no QQ adapter code.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-06`.

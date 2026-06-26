## [2026-06-26 14:46] | Task: serve channel conversation dispatch

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local CLI, xmake release build`
- Linked plan: none - this is a small hardening slice under the shipped
  `--serve` channel owner.

### User Query

> Continue advancing the project implementation.

### Changes Overview

- Areas: `oran-bootstrap` service mode, channel ingress dispatch.
- Key actions:
  - Changed `bootstrap::serve_channels(...)` dispatch from one sequential fan-in
    loop to per-channel+conversation worker queues keyed by
    `(channel_id, conversation_id)`.
  - Preserved in-order handling for messages in the same conversation while
    allowing unrelated conversations to await agent runs concurrently.
  - Kept the slice-258 `channel:<channel_id>` triggered automation enqueue
    wrapper in front of the direct reply path.
  - Added a focused bootstrap regression showing a slow `conv-1` first reply
    does not block `conv-2`, while `conv-1`'s second message still waits.

### Design Intent

Slice 256 made `--serve` the first long-lived channel fan-in owner, but its
dispatcher awaited each message end-to-end before receiving the next one. That
made same-conversation ordering trivially correct, but it also let one slow
conversation block every other configured channel conversation.

This slice keeps scheduling policy at the process-owner boundary rather than
moving it into `oran-channel::ChannelManager`. The manager remains a generic
fan-in/send primitive; `serve_channels` owns the daemon concern and therefore
owns the per-conversation queues, child cancellation, and shutdown drain. The
bounded queue capacity is intentionally local for now. Per-message deadlines,
idle worker eviction, and per-agent strand splitting remain separate slices.

During focused testing, an awaited worker-done send could race parent cleanup:
the parent received the done token and destroyed the worker cancellation signal
before the worker's own send awaiter resumed and cleared its cancellation slot.
The final completion protocol uses an atomic completed-worker count plus a
non-blocking progress token, so a worker does not await after announcing
completion.

### Files Modified

- `include/oran/bootstrap/serve.hpp`
- `src/oran-bootstrap/serve.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_serve.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 259 snapshot, history pointer, counts, and next step.
- `docs/ROADMAP.md` - Channels / Agent loop / Tool scheduler frontiers updated.
- `docs/ARCHITECTURE.md` - bootstrap/channel ownership notes updated.
- `docs/design-docs/bootstrap-runtime.md` - per-conversation worker behavior and
  shutdown protocol recorded.
- `docs/design-docs/channel-abstraction.md` - per-conversation serialization
  status updated.
- `docs/product-specs/0003-multi-platform-channels.md` - shipped channel ordering
  note.
- `docs/QUALITY_SCORE.md` - bootstrap test count and channel coverage updated.
- `docs/releases/feature-release-notes.md` - slice 259 release note.

### Validation

- Commands run:
  - `xmake f -m debug --sanitizers=y`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels dispatches mock inbound messages and replies" --reporter compact --success`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels serializes each conversation without blocking others" --reporter compact --success`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels enqueues matching automation triggers before replying" --reporter compact --success`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels stops gracefully on parent cancellation" --reporter compact --success`
  - `xmake f -m release`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap --reporter compact`
  - `xmake test`
  - `xmake run orangutan -- --help`
  - `xmake f -m release --channel_qq=y`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap --reporter compact`
  - `make ci`
- Tests added/changed:
  - `tests/bootstrap/test_serve.cpp` adds
    `serve_channels serializes each conversation without blocking others`.
- Bench impact: none; this changes daemon scheduling policy, not a hot library
  primitive.
- Compile-budget delta: not measured; the change is private to
  `src/oran-bootstrap/serve.cpp` and one focused bootstrap test.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md` row
  `serve-channel-conversation-dispatch`.

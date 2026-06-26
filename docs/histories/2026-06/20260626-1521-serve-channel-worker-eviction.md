## [2026-06-26 15:21] | Task: serve channel worker eviction

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local CLI, xmake debug/release builds`
- Linked plan: none - small hardening slice under the shipped `--serve`
  channel owner.

### User Query

> Continue advancing the project implementation.

### Changes Overview

- Areas: `oran-bootstrap` service mode, channel ingress dispatch.
- Key actions:
  - Added `ServeChannelOptions` with `conversation_queue_capacity` and
    `conversation_idle_ttl` for the C++ owner/test boundary.
  - Changed each per-channel+conversation worker to exit after its inbox is
    empty for the idle TTL.
  - Had workers set a completion flag and wake the dispatcher non-blockingly;
    the dispatcher erases completed workers on progress wakes and before
    enqueueing later messages.
  - Added focused bootstrap coverage proving an idle worker wakes cooperative
    stop after the first message and invalid worker options fail up front.

### Design Intent

Slice 259 fixed head-of-line blocking by giving each `(channel_id,
conversation_id)` key its own worker, but a long-lived service would retain one
worker record for every historical conversation. This slice keeps scheduling
policy at the `--serve` owner boundary and bounds that table by active/recent
conversations instead. The defaults stay code-owned for now: 64 queued messages
per conversation and a 5-minute idle TTL. No JSON `serve` config field was added
because the typed config block is still deferred until more than one concern
needs operator tuning.

The shutdown protocol now waits on per-worker completion flags instead of a
total completed-worker counter. That matters once idle workers can be erased:
historical completions must not make active shutdown waits appear complete.

### Files Modified

- `include/oran/bootstrap/serve.hpp`
- `src/oran-bootstrap/serve.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_serve.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 260 snapshot, history pointer, counts, and next step.
- `docs/ROADMAP.md` - Channels frontier and dependency frontier updated.
- `docs/ARCHITECTURE.md` - channel/bootstrap ownership notes updated.
- `docs/design-docs/bootstrap-runtime.md` - options, idle eviction, and shutdown
  protocol recorded.
- `docs/design-docs/channel-abstraction.md` - per-conversation serialization
  status and remaining mitigations updated.
- `docs/product-specs/0003-multi-platform-channels.md` - shipped idle eviction
  note.
- `docs/QUALITY_SCORE.md` - bootstrap test counts and channel coverage updated.
- `docs/releases/feature-release-notes.md` - slice 260 release note.

### Validation

- Commands run:
  - `xmake f -m debug --sanitizers=y --channel_qq=n`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels evicts idle conversation workers before cooperative stop" --reporter compact --success`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels rejects invalid channel worker options" --reporter compact --success`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels serializes each conversation without blocking others" --reporter compact --success`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels enqueues matching automation triggers before replying" --reporter compact --success`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels stops gracefully on parent cancellation" --reporter compact --success`
  - `xmake f -m release --channel_qq=n`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap --reporter compact`
  - `xmake test`
  - `xmake run orangutan -- --help`
  - `xmake f -m release --channel_qq=y`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap --reporter compact`
  - `xmake f -m release --channel_qq=n`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - `tests/bootstrap/test_serve.cpp` adds
    `serve_channels evicts idle conversation workers before cooperative stop`
    and `serve_channels rejects invalid channel worker options`.
- Bench impact: none; this is daemon state bounding rather than a hot library
  primitive.
- Compile-budget delta: not measured; the public header adds a small options
  struct over existing `<chrono>/<cstddef>` includes, while implementation work
  stays in `src/oran-bootstrap/serve.cpp`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md` row
  `serve-channel-worker-eviction`.

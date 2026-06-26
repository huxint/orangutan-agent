## [2026-06-26 15:48] | Task: serve channel worker metrics

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local CLI, xmake debug/release builds`
- Linked plan: none - small hardening slice under the shipped `--serve`
  channel owner.

### User Query

> Continue advancing the project implementation.

### Changes Overview

- Areas: `oran-bootstrap` service mode, channel ingress dispatch observability.
- Key actions:
  - Added `ServeChannelWorkerMetrics` to report active/max conversation workers,
    worker lifecycle counts, idle evictions, message enqueues, sent replies, and
    failure counters for one `serve_channels(...)` run.
  - Added `ServeChannelOptions::metrics_observer`, a synchronous C++ owner/test
    observer called from the dispatcher executor after worker-table and message
    progress changes.
  - Kept worker-table counters dispatcher-owned while worker-side reply/failure
    counters use atomics, so embedders that drive `serve_channels` on a
    non-strand executor still receive coherent snapshots.
  - Caught observer exceptions and reported them instead of letting an embedding
    metrics callback terminate the daemon.

### Design Intent

Slice 260 bounded the conversation-worker table, but long-lived owners still had
no structured view of that state. This slice adds the smallest complete
observability seam at the same owner boundary: the low-level `ChannelManager`
stays policy-free, and `--serve` can now expose worker metrics to tests or a
future operator-facing sink. No JSON config field, metrics endpoint, or hook
payload was added; those need a broader daemon metrics design.

### Files Modified

- `include/oran/bootstrap/serve.hpp`
- `src/oran-bootstrap/serve.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_serve.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 261 snapshot, history pointer, counts, and next step.
- `docs/ROADMAP.md` - Channels frontier and dependency frontier updated.
- `docs/ARCHITECTURE.md` - channel/bootstrap ownership notes updated.
- `docs/design-docs/bootstrap-runtime.md` - metrics observer and counter
  ownership recorded.
- `docs/design-docs/channel-abstraction.md` - per-conversation serialization
  status and remaining mitigations updated.
- `docs/product-specs/0003-multi-platform-channels.md` - shipped metrics note.
- `docs/QUALITY_SCORE.md` - bootstrap counts and channel coverage updated.
- `docs/releases/feature-release-notes.md` - slice 261 release note.

### Validation

- Commands run:
  - `xmake f -m debug --sanitizers=y --channel_qq=n`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels reports conversation worker metrics" --reporter compact --success`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels keeps serving when worker metrics observer throws" --reporter compact --success`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels evicts idle conversation workers before cooperative stop" --reporter compact --success`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels serializes each conversation without blocking others" --reporter compact --success`
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
    `serve_channels reports conversation worker metrics` and
    `serve_channels keeps serving when worker metrics observer throws`.
- Bench impact: none; this is service-owner observability plumbing, not a hot
  library primitive.
- Compile-budget delta: not measured; the public header adds a small POD metrics
  struct and one `std::function` field to an existing options struct.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md` row
  `serve-channel-worker-metrics`.

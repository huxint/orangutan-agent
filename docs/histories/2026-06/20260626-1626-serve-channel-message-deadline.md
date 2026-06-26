## [2026-06-26 16:26] | Task: serve channel message deadline

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local CLI, xmake debug/release builds`
- Linked plan: none - small hardening slice under the shipped `--serve`
  channel owner.

### User Query

> Continue advancing the project implementation.

### Changes Overview

- Areas: `oran-bootstrap` service mode, channel ingress dispatch reliability.
- Key actions:
  - Added `ServeChannelOptions::message_deadline`, an optional positive C++
    owner/test deadline for one routed channel agent/reply send attempt.
  - On deadline expiry, cancel the in-flight attempt and send a fixed
    still-working fallback reply on the same inbound message envelope.
  - Extended `ServeChannelWorkerMetrics` with `message_timeouts`; successful
    fallback sends count as replies, and failed fallback sends count as dispatch
    failures.

### Design Intent

Slices 259-261 made long-lived channel dispatch concurrent, bounded, and
observable, but a single routed agent turn could still run indefinitely inside a
conversation worker. This slice adds the smallest complete deadline seam at the
same bootstrap owner boundary. The low-level `oran-channel` manager still owns
only fan-in/send primitives, while `--serve` owns scheduling policy. No JSON
config field or durable later-reply rejoin was added; both need a broader typed
`serve`/channel config and background rejoin design.

### Files Modified

- `include/oran/bootstrap/serve.hpp`
- `src/oran-bootstrap/serve.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_serve.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 262 snapshot, history pointer, counts, and next step.
- `docs/ROADMAP.md` - Channels frontier and dependency frontier updated.
- `docs/ARCHITECTURE.md` - channel/bootstrap ownership notes updated.
- `docs/design-docs/bootstrap-runtime.md` - deadline fallback behavior recorded.
- `docs/design-docs/channel-abstraction.md` - shipped deadline seam and remaining
  durable rejoin/config work recorded.
- `docs/product-specs/0003-multi-platform-channels.md` - shipped deadline note.
- `docs/RELIABILITY.md` - timeout row corrected to the C++ owner/test deadline
  seam.
- `docs/QUALITY_SCORE.md` - bootstrap counts and channel coverage updated.
- `docs/releases/feature-release-notes.md` - slice 262 release note.

### Validation

- Commands run:
  - `xmake f -m debug --sanitizers=y --channel_qq=n`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels sends still-working reply when message deadline expires" --reporter compact --success`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels rejects invalid channel worker options" --reporter compact --success`
  - `build/linux/x86_64/debug/test-bootstrap "serve_channels reports conversation worker metrics" --reporter compact --success`
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
    `serve_channels sends still-working reply when message deadline expires`,
    extends the metrics test with `message_timeouts`, and extends invalid
    options coverage for zero/negative message deadlines.
- Bench impact: none; this is service-owner deadline/fallback plumbing, not a
  hot library primitive.
- Compile-budget delta: not measured; the public header adds one optional
  duration and one counter field.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md` row
  `serve-channel-message-deadline`.

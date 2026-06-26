## [2026-06-26 17:02] | Task: serve channel metrics log sink

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `local CLI, xmake release build`
- Linked plan: none - small follow-up under the shipped `--serve` owner.

### User Query

> Continue advancing the project implementation; keep future direction balanced
> across tracks rather than over-weighting channels.

### Changes Overview

- Areas: `oran-bootstrap` service mode, channel observability.
- Key actions:
  - Added `format_serve_channel_worker_metrics(...)` plus
    `ServeChannelMetricsLogSink` for deduplicated one-line worker metrics logs.
  - Wired `run_serve` to install that sink whenever configured channel adapters
    are active.
  - Extended bootstrap tests so the sink format/deduplication and the existing
    `serve_channels` metrics observer path are covered together.

### Design Intent

Slices 261-262 exposed channel worker metrics and deadline counters at the C++
owner/test boundary, but ordinary `orangutan --serve` operators still needed a
custom embedder observer to see them. This slice keeps the smallest daemon-visible
surface: stderr log lines, no JSON config, no HTTP endpoint, and no policy moved
into `oran-channel`. Per the user's balancing note, the next implementation slice
should preferably rotate to a non-channel track unless a channel blocker is clearer.

### Files Modified

- `include/oran/bootstrap/serve.hpp`
- `src/oran-bootstrap/serve.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_serve.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 263 snapshot, counts, next-step balancing note.
- `docs/ROADMAP.md` - Channels frontier and dependency frontier updated.
- `docs/ARCHITECTURE.md` - bootstrap/channel ownership notes updated.
- `docs/design-docs/bootstrap-runtime.md` - metrics sink behavior recorded.
- `docs/design-docs/channel-abstraction.md` - bootstrap-owned metrics sink noted.
- `docs/product-specs/0003-multi-platform-channels.md` - shipped sink note.
- `docs/RELIABILITY.md` - metrics/logging note updated.
- `docs/QUALITY_SCORE.md` - bootstrap counts and channel coverage updated.
- `docs/releases/feature-release-notes.md` - slice 263 release note.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap "ServeChannelMetricsLogSink emits deduplicated channel worker metrics" --reporter compact --success`
  - `build/linux/x86_64/release/test-bootstrap "serve_channels reports conversation worker metrics" --reporter compact --success`
  - `build/linux/x86_64/release/test-bootstrap "serve_channels sends still-working reply when message deadline expires" --reporter compact --success`
  - `build/linux/x86_64/release/test-bootstrap --reporter compact`
  - `xmake run orangutan -- --help`
  - `xmake f -m release --channel_qq=y`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap --reporter compact`
  - `xmake f -m release --channel_qq=n`
  - `xmake test`
  - `make ci`
- Tests added/changed:
  - Added `ServeChannelMetricsLogSink emits deduplicated channel worker metrics`.
  - Extended `serve_channels reports conversation worker metrics` to prove the
    observer snapshots format through the same sink.
- Bench impact: none; this is daemon log formatting around existing metrics
  snapshots.
- Compile-budget delta: one small public helper/sink in `oran-bootstrap`; no new
  dependency and no heavy public include.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md` row
  `serve-channel-metrics-log-sink`.

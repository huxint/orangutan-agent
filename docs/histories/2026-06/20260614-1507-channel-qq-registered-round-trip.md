## [2026-06-14 15:07] | Task: channel-qq registered round-trip

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI/API, xmake release builds
- Linked plan: `docs/exec-plans/active/2026-06-10-channel-qq-port.md`

### User Query

Continue the long-running slice workflow with an effectively unbounded budget:
read the development docs first, understand the real project progress, choose
the highest-value current slice instead of inventing a status-only next slice,
then complete implementation, validation, docs/history sync, and a detailed
Conventional Commit.

### Changes Overview

- Areas: gated QQ channel bootstrap integration, `test-bootstrap`, channel
  docs, release/status docs.
- Key actions:
  - Added an enabled-only bootstrap round-trip test for configured QQ channels:
    `register_configured_channels(...)` assembles a real QQ adapter stack from
    config, `ChannelManager::start_all()` starts it, one scripted gateway
    message is received, `dispatch_one(...)` routes it through the configured
    agent bridge, and the reply is sent back through the QQ passive-reply API.
  - Validated the side effects the production path promises: the provider sees
    the QQ text as user input, the configured agent overlay is rendered, the
    trace repository records a turn for the routed agent, `audit.db` exists,
    the gateway Identify payload carries the resolved token, and the outbound
    QQ API request preserves `content`, `msg_id`, and `msg_seq`.
  - Bumped the binary slice tag to `2.0.0-slice236`.

### Design Intent

This is QQ-port milestone 4a: the mock-server CI acceptance for the
registered QQ path. It proves the slice-235 construction seam is not just
registerable, but can carry one inbound QQ gateway dispatch through the same
manager, routed prompt runner, agent prompt bridge, trace/audit plumbing, and
passive reply sender that a real configured channel will use.

It deliberately does not mark milestone 4 complete. CI still uses loopback
scripted HTTP/WebSocket fixtures rather than real QQ credentials, and the
adapter option remains default-off until a manual or nightly real-credential
smoke gate proves the same path against the platform.

### Files Modified

- `tests/bootstrap/test_channel_ingress.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped the slice to 236, pointed at this history,
  refreshed the enabled bootstrap test count, and recorded the remaining
  real-credential smoke gate as the next concrete channel slice.
- `docs/ROADMAP.md` - moved the Channels frontier to QQ milestone 4a and kept
  the manual/nightly real-credential gate as the next step.
- `docs/exec-plans/active/2026-06-10-channel-qq-port.md` - split milestone 4
  into mock CI acceptance and real-credential smoke, then marked 4a complete.
- `docs/design-docs/channel-abstraction.md` - documented the registered-path
  mock round-trip proof and the remaining default-on gate.
- `docs/product-specs/0003-multi-platform-channels.md` - added the slice-236
  shipped state while leaving the full QQ acceptance criterion open.
- `docs/QUALITY_SCORE.md` - refreshed the bootstrap enabled count and channel
  quality frontier.
- `docs/releases/feature-release-notes.md` - added the user-visible slice 236
  release note.

### Validation

- Commands run:
  - `git diff --check`
  - `xmake f -m release --channel_qq=y --ccache=n`
  - `xmake clean test-bootstrap && xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap "[channel-qq]" --reporter=console --verbosity=normal`
  - `build/linux/x86_64/release/test-bootstrap --reporter=console --verbosity=normal`
  - `xmake build test-channel-qq && build/linux/x86_64/release/test-channel-qq --reporter=console --verbosity=normal`
  - `xmake f -m release --channel_qq=n --ccache=n`
  - `xmake clean test-bootstrap && xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap --reporter=console --verbosity=normal`
  - `xmake build orangutan && xmake run orangutan -- --help`
  - `make ci`
- Tests added/changed:
  - `test-bootstrap` adds an enabled-only `[channel-qq]` registered-path
    round-trip test covering bootstrap QQ registration, gateway Identify,
    gateway C2C receive, routed agent prompt execution, trace row persistence,
    `audit.db` creation, and QQ passive reply request shape.
  - Focused `[channel-qq]` filter: 3 cases / 48 assertions.
  - Enabled `test-bootstrap`: 148 cases / 1352 assertions.
  - Default `test-bootstrap`: 146 cases / 1312 assertions.
  - Gated `test-channel-qq`: 55 cases / 344 assertions.
- Bench impact:
  - None. This is integration acceptance coverage over existing channel and
    bootstrap paths; no new hot-path implementation or benchmarkable
    alternative was introduced.
- Compile-budget delta:
  - No budget row changed. The optional QQ adapter remains default-off, and the
    default `--channel_qq=n` build still compiles `test-bootstrap` without
    linking `oran-channel-qq`.
  - Validation used `--ccache=n` for both enabled and default configurations to
    avoid reusing stale objects after an interrupted local xmake build had left
    zero-byte outputs in the cache.
  - Rebuilding `test-bootstrap` emitted the existing GCC
    `-Wmaybe-uninitialized` warning in
    `tests/bootstrap/test_automation_prompt_runner.cpp:369`; it is outside this
    slice and the builds completed successfully.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
- Next plan step: run the QQ registered path against real QQ credentials in a
  manual/nightly smoke gate before considering `channel_qq` default-on.

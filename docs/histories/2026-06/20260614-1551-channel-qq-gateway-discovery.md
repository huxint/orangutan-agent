## [2026-06-14 15:51] | Task: channel-qq gateway discovery

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI/API, xmake release builds
- Linked plan: `docs/exec-plans/active/2026-06-10-channel-qq-port.md`

### User Query

Continue the long-running slice workflow with an effectively unbounded budget:
read the development docs first, understand real project progress, choose the
highest-value current slice instead of inventing a status-only next slice,
then complete implementation, validation, docs/history sync, and a detailed
Conventional Commit.

### Changes Overview

- Areas: gated QQ channel adapter, hidden QQ real-smoke setup, channel docs,
  reliability docs, release/status docs.
- Key actions:
  - Added typed `GET /gateway/bot` discovery support to `oran-channel-qq`:
    `GatewayBotInfo`, `GatewaySessionStartLimit`,
    `parse_gateway_bot_response(...)`, and `discover_gateway_bot(...)`.
  - The parser validates `ws://` / `wss://` gateway URLs, positive shard
    counts, and non-negative `session_start_limit` fields without exposing
    `nlohmann::json` in the public header.
  - Updated the hidden registered-path real-smoke test so
    `ORAN_TEST_QQ_GATEWAY_URL` is optional. If absent, the test discovers the
    current bot gateway through the authenticated QQ API before building the
    same config-driven adapter stack.
  - Bumped the binary slice tag to `2.0.0-slice238`.

### Design Intent

Slice 237 made the real-smoke gate executable, but local 4b-ii evidence is
still blocked by missing QQ credentials and a sendable operator conversation.
The highest-value unblocked slice was to remove one avoidable setup burden for
that future run: operators should not have to manually obtain and copy a
transient gateway WebSocket URL when QQ already exposes `/gateway/bot`.

The discovery helper lands in `oran-channel-qq` rather than directly changing
bootstrap registration. `register_configured_channels(...)` is synchronous, so
production async discovery needs a later ownership decision; the hidden smoke
can safely discover first, then create the existing config with an explicit
`qq_gateway_url`.

This does not claim the real smoke has passed. The local environment still has
no QQ real-smoke variables set, so `channel_qq` remains default-off and 4b-ii
remains open.

### Files Modified

- `include/oran/channel-qq/api_client.hpp`
- `src/oran-channel-qq/api_client.cpp`
- `tests/channel-qq/test_api_client.cpp`
- `tests/bootstrap/test_channel_ingress.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped the slice to 238, recorded gateway discovery, and
  kept the credentialed real-smoke run open.
- `docs/ROADMAP.md` - moved the Channels frontier to 4b-ii prep and left the
  credentialed run as next.
- `docs/exec-plans/active/2026-06-10-channel-qq-port.md` - added the 4b-ii
  prep step and decision log entry.
- `docs/design-docs/channel-abstraction.md` - documented the gateway discovery
  helper and smoke fallback behavior.
- `docs/product-specs/0003-multi-platform-channels.md` - added the slice-238
  shipped state while leaving QQ acceptance criterion 2 open.
- `docs/RELIABILITY.md` - updated the real-smoke command and env table so
  `ORAN_TEST_QQ_GATEWAY_URL` is an optional override.
- `docs/QUALITY_SCORE.md` - refreshed the gated `oran-channel-qq` count and
  channel frontier.
- `docs/releases/feature-release-notes.md` - added the slice 238 release note.

### Validation

- Commands run:
  - `git diff --check` - passed.
  - `xmake f -m release --channel_qq=y --ccache=n` - passed.
  - `xmake build test-channel-qq` - passed.
  - `build/linux/x86_64/release/test-channel-qq --reporter=console --verbosity=normal`
    - passed, 58 cases / 369 assertions.
  - `xmake clean test-bootstrap` - passed.
  - `xmake build test-bootstrap` under `--channel_qq=y` - passed. GCC still
    emits the existing `tests/bootstrap/test_automation_prompt_runner.cpp:369`
    `-Wmaybe-uninitialized` warning; no new warning is tied to this slice.
  - `build/linux/x86_64/release/test-bootstrap "[channel-qq]" --reporter=console --verbosity=normal`
    - passed, 4 cases / 49 assertions.
  - `build/linux/x86_64/release/test-bootstrap "registered QQ channels real-smoke one operator message through the routed prompt path" --reporter=console --verbosity=normal`
    - passed, 1 case / 1 assertion. This still proves only the explicit
    no-credential no-op command, not a real QQ pass.
  - `build/linux/x86_64/release/test-bootstrap --reporter=console --verbosity=normal`
    under `--channel_qq=y` - passed, 148 cases / 1352 assertions.
  - `xmake f -m release --channel_qq=n --ccache=n` - passed.
  - `xmake clean test-bootstrap` - passed.
  - `xmake build test-bootstrap` under the default build - passed with the
    same existing GCC warning noted above.
  - `build/linux/x86_64/release/test-bootstrap --reporter=console --verbosity=normal`
    under the default build - passed, 146 cases / 1312 assertions.
  - `xmake build orangutan` - passed.
  - `xmake run orangutan -- --help` - passed and reported
    `orangutan v2.0.0-slice238`.
  - QQ smoke env check - all `ORAN_TEST_QQ_*` smoke variables were unset
    locally, so no credentialed real-network QQ pass is claimed.
  - `make ci` - passed.
- Tests added/changed: three gated `test-channel-qq` API cases for discovery
  request/parse/defaults/error handling, plus the hidden `test-bootstrap`
  smoke now discovers a gateway URL when none is supplied.
- Bench impact: none; this is setup/validation-path correctness, not a hot
  runtime path.
- Compile-budget delta: the public header adds small stdlib value types; JSON
  parsing stays in the gated `oran-channel-qq` implementation. Default
  `--channel_qq=n` builds still compile no QQ adapter code.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
- Next plan step: run the hidden real QQ smoke test with real credentials and
  a sendable operator conversation, then record the result before considering
  `channel_qq` default-on.

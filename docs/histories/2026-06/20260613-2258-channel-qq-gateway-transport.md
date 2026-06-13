## [2026-06-13 22:58] | Task: channel-qq gateway transport driver

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan:
  `docs/exec-plans/active/2026-06-10-channel-qq-port.md`

### User Query

> Continue iterating the project slice-by-slice after reading the development
> docs and true current progress; pick the highest-value current slice, keep
> progress docs honest, validate, and commit with a detailed conventional
> message.

### Changes Overview

- Areas: `oran-channel-qq` gains the QQ-port milestone 2b-ii gateway driver:
  `qq::GatewayTransport`, a caller-owned layer over the slice-231
  `http::WebSocket` primitive and the slice-230 `qq::GatewaySession` state
  machine.
- Key actions: added `<oran/channel-qq/gateway_transport.hpp>` and
  `src/oran-channel-qq/gateway_transport.cpp`; exposed the header through the
  QQ umbrella; added loopback WebSocket tests covering Identify, heartbeat,
  Resume, auth-close token refresh, token failure propagation, and
  cancellation; bumped the binary slice tag to `2.0.0-slice232`.

### Design Intent

This completes QQ-port milestone 2b without crossing into the milestone-3
trait adapter. `GatewayTransport::next_dispatch()` returns one non-lifecycle
`GatewayDispatch` per caller resume, leaving dispatch-to-`InboundMessage`
parsing, outbound sends, capabilities, and bootstrap registration to
`QqChannel`. That keeps the network/session/reconnect risk isolated in the
gated `oran-channel-qq` target and preserves the current dependency boundary:
`oran-channel` still joins only when the trait adapter exists.

The driver is caller-owned and cancel-aware. It does not start a detached
receive task or heartbeat task; instead it races `WebSocket::receive(...)`
against the next heartbeat deadline using `async::sleep_for`, sends heartbeat
payloads through `GatewaySession::build_heartbeat()`, and re-enters receive
until a real dispatch arrives. Close-code recovery follows the SDK-grounded
table already pinned in slice 230: 4009 resumes, ordinary/fresh closes reset
continuity, 4004 invalidates the cached token before reconnect, 4010-4014
surface config errors, and 4914/4915 are fatal. Token/config failures surface
directly and are not treated as retryable reconnect control flow.

### Files Modified

- `include/oran/channel-qq/gateway_transport.hpp` (new)
- `src/oran-channel-qq/gateway_transport.cpp` (new)
- `include/oran/channel-qq.hpp`
- `tests/channel-qq/test_gateway_transport.cpp` (new)
- `src/oran-bootstrap/bootstrap.cpp` (slice tag -> slice 232)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumps snapshot to slice 232, refreshes the
  `oran-channel-qq` test surface, and names milestone 3 as the next frontier.
- `docs/ROADMAP.md` — moves the Channels row frontier to the shipped gateway
  transport driver.
- `docs/ARCHITECTURE.md` — extends the `oran-channel-qq` inventory row with
  `GatewayTransport`.
- `docs/design-docs/channel-abstraction.md` — records QQ milestone 2b-ii under
  "QQ Adapter Migration".
- `docs/product-specs/0003-multi-platform-channels.md` — adds slice 232 to
  "Shipped So Far".
- `docs/QUALITY_SCORE.md` — refreshes `oran-channel-qq` test counts and the
  Channels row.
- `docs/exec-plans/active/2026-06-10-channel-qq-port.md` — checks off
  milestone 2b-ii and records the slice-232 decision.
- `docs/releases/feature-release-notes.md` — adds the slice 232 release note.

### Validation

- Commands run:
  - `xmake f --channel_qq=y && xmake build test-channel-qq && xmake run test-channel-qq`
    — **45 cases / 249 assertions**.
  - `xmake build bench-channel-qq` — bench bucket builds under the gated QQ
    option.
  - `xmake f --channel_qq=n && xmake build orangutan` — default-disabled path
    still builds without the adapter target.
  - `xmake run orangutan -- --help` — reports `orangutan v2.0.0-slice232`.
  - `make ci` — docs scaffold, hygiene, docs-sync placeholders,
    `STATUS.md` freshness, dependency layering, prompt preamble, action pinning.
- Tests added/changed:
  - New `test_gateway_transport.cpp`: Identify plus heartbeat over the loopback
    WebSocket server, resumable close 4009, auth close 4004 with token refresh,
    token-endpoint failure propagation, and cancellation while suspended.
- Bench impact:
  - None added. This slice is network/timer correctness, not a hot-path data
    structure comparison; existing `bench-channel-qq` still covers gateway
    frame consumption cost.
- Compile-budget delta:
  - One new gated `oran-channel-qq` TU and one test TU. No new third-party
    dependency, and disabled `--channel_qq=n` builds still compile zero adapter
    code.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
- Next slice: QQ-port milestone 3 — first `QqChannel` trait-adapter slice over
  `GatewayTransport` and `ApiClient`.

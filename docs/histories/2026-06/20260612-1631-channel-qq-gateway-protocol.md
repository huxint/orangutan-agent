## [2026-06-12 16:31] | Task: channel-qq gateway protocol/session state machine

### Execution Context

- Agent: `Claude Code`
- Base model: `Opus 4.8 (1M context)`
- Runtime: `Claude Code CLI`
- Linked plan:
  `docs/exec-plans/active/2026-06-10-channel-qq-port.md`

### User Query

> Deeply understand the project architecture and implementation objectives,
> then continuously iterate on the project, following the development
> documentation; ship a slice as a commit.

### Changes Overview

- Areas: `oran-channel-qq` gains the pure gateway protocol/session state
  machine (`<oran/channel-qq/gateway.hpp>`), the offline-testable first half
  of the QQ-port milestone 2 receive transport. No build-wiring, dependency,
  or public-trait changes beyond the new header in the existing gated target.
- Key actions: added `qq::GatewaySession` (decode one gateway text frame at a
  time, advance Hello → Identify/Resume, Dispatch → READY/RESUMED capture vs.
  surfaced `GatewayDispatch`, Reconnect/Invalid-Session recovery directives,
  seq caching, heartbeat-payload building) returning a `GatewayReaction`
  struct of independent directives; the free `classify_close_code` function
  mapping WebSocket close codes onto a `CloseRecovery` action; and the
  `GatewayOpcode` enum. Frames and outbound payloads cross the boundary as
  serialized JSON strings (C6); the auth token is injected into
  `build_identify`/`build_resume` so the session never stores a secret (C5).

### Design Intent

This is **milestone 2a**: the protocol/session half of the receive transport,
validated offline before any network code — the same discipline milestone 1
applied to the API client. The roadmap/plan named milestone 2 as one slice,
but it mixes two very different risk profiles: a pure, deterministic state
machine (this slice) and a persistent `wss://` transport under
`async::Runtime`. The codebase has **no WebSocket primitive** — `oran-http`
is body + SSE only — and the legacy `qq-transport.cpp` did it with raw
`curl_ws_*` on a dedicated `std::thread`, which violates C2 (no `std::thread`)
and C6 (no curl in headers). Building the transport right means first
extending `oran-http` with a cancel-aware WebSocket boundary, which is its own
slice (2b). Splitting here keeps this slice well under the C14 budget and puts
the *high-value correctness work* — the opcode dance and the close-code
classification the QQ wiki and botpy get wrong — behind exhaustive offline
tests.

The state machine is **mirror-grounded** against Tencent's botgo/botpy SDKs
via `docs/references/messaging-platform-apis.md` §2.2, not the legacy code,
because the reference *corrects* the legacy: `classify_close_code` encodes
4009 as the only resume-able unexpected close, 4013/4014 as intents problems
(`fix_config`), and 4914/4915 as fatal (offline/banned) — the legacy channel
had no close-code handling at all and the doc-only research pass had 4914/4915
wrong. Default intents are `GROUP_AND_C2C (1<<25) | INTERACTION (1<<26)` —
the single bit gating both group and C2C private messages, plus keyboard
callbacks — rather than the legacy's guild-era union, since an unauthorized
intent closes the socket with 4014. `GatewayReaction` is a struct of
independent directives (one Hello frame both arms the heartbeat timer and
sends Identify/Resume) so the transport owner stays a thin mechanical driver
and all decisions remain testable in isolation. `GatewaySession` is
deliberately not thread-safe: the transport owner drives `consume` from a
single coroutine, the one-resume-per-call discipline the `Channel` trait
already expects.

### Files Modified

- `include/oran/channel-qq/gateway.hpp` (new)
- `src/oran-channel-qq/gateway.cpp` (new)
- `include/oran/channel-qq.hpp` (umbrella header now includes gateway.hpp)
- `tests/channel-qq/test_gateway.cpp` (new)
- `bench/channel-qq/scenarios/gateway_consume.cpp` (new)
- `bench/channel-qq/main.cpp` (registers the gateway-consume scenario)
- `src/oran-bootstrap/bootstrap.cpp` (slice tag → slice 230)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumps the snapshot to slice 230, refreshes the
  `oran-channel-qq` surface count, splits milestone 2 into 2a (shipped) /
  2b (next: the `oran-http` WebSocket transport + driver).
- `docs/ROADMAP.md` — moves the Channels row frontier to the shipped gateway
  protocol machine and names the WebSocket network transport as the next step.
- `docs/ARCHITECTURE.md` — extends the `oran-channel-qq` inventory row with
  the gateway protocol surface.
- `docs/design-docs/channel-abstraction.md` — records milestone 2a under
  "QQ Adapter Migration".
- `docs/product-specs/0003-multi-platform-channels.md` — adds slice 230 to
  "Shipped So Far".
- `docs/QUALITY_SCORE.md` — refreshes the channel-qq test count.
- `docs/exec-plans/active/2026-06-10-channel-qq-port.md` — splits milestone 2
  into 2a/2b, checks off 2a, and records the slice-230 decision log.
- `docs/releases/feature-release-notes.md` — adds the slice 230 release note.

### Validation

- Commands run:
  - `xmake f -m release --channel_qq=y && xmake build oran-channel-qq`
  - `xmake build test-channel-qq && xmake run test-channel-qq` (40 cases /
    209 assertions; 19 new gateway cases / 80 assertions)
  - `xmake build bench-channel-qq && xmake run bench-channel-qq`
  - `xmake -j$(nproc)` (full build, option on) + `xmake test` (all 19
    buckets passed)
  - `xmake f --channel_qq=n && xmake show -l targets` (zero channel-qq
    targets configured)
  - `make ci`
- Tests added/changed:
  - New `test_gateway.cpp`: Hello → Identify (fresh) and Resume (after a
    restored session), heartbeat-interval int-or-string + default fallback,
    seq caching across frames, READY/RESUMED session capture vs. surfaced
    dispatch, heartbeat solicit/ack, Reconnect → resume, Invalid-Session
    bare-bool true/false, `build_identify`/`build_resume`/`build_heartbeat`
    payload shapes (incl. null `d` before first seq), `can_resume` /
    `restore` / `reset`, non-object-frame parsing error, unknown-opcode
    empty reaction, and the full `classify_close_code` table.
- Bench impact:
  - New `gateway_consume` A-vs-B: heartbeat-ack frame (~243 ns, decode +
    empty reaction) vs. dispatch frame (~1.63 µs, decode + seq cache + `d`
    re-serialization) locally — justifies returning lifecycle frames before
    paying the dispatch re-serialization.
- Compile-budget delta:
  - One new TU on the existing gated `oran-channel-qq` row (1.2 s / 2.5 s /
    3.0 s); no new third-party dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
- Next slice: milestone 2b — extend `oran-http` with a cancel-aware
  WebSocket boundary, then drive `GatewaySession` + the heartbeat timer +
  reconnect backoff behind `Channel::next_message()`.

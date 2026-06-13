## [2026-06-13 13:57] | Task: oran-http cancel-aware WebSocket boundary

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

- Areas: `oran-http` gains its first WebSocket primitive
  (`<oran/http/websocket.hpp>`), the `wss://` transport the QQ gateway
  (`qq::GatewaySession`, slice 230) needs but the codebase lacked — the client
  was body + SSE only. No new dependency (libcurl + asio already present); no
  public-trait changes beyond the new header and its umbrella include.
- Key actions: added `http::WebSocket` (`connect` / `receive` / `send_text` /
  `close`, plus `WsConnectRequest` / `WsMessage` / `WsMessageKind`) over
  libcurl's connect-only WebSocket mode (`CURLOPT_CONNECT_ONLY = 2`); extracted
  the shared libcurl RAII wrappers + error translation from `client.cpp` into
  `src/oran-http/_impl/curl_common.hpp` so the two transport TUs share one
  copy (C6: curl stays out of headers). Added a scripted loopback WebSocket
  test server (`tests/test-helpers/ws_test_server.hpp`, real RFC 6455
  handshake) and `tests/http/test_websocket.cpp` (12 cases / 88 assertions).

### Design Intent

This is **milestone 2b-i**: the `oran-http` WebSocket primitive, split out of
milestone 2b exactly as slice 230's decision log anticipated ("extending
`oran-http` with a cancel-aware WebSocket boundary ... is its own slice"). The
driver half — wiring `GatewaySession` + the heartbeat timer + reconnect backoff
behind `Channel::next_message()` in `oran-channel-qq` — is milestone 2b-ii.
Splitting keeps each slice in one library with one risk profile, mirroring the
2a/2b split.

The boundary is **cancel-aware without a thread** (C2/C11). The legacy QQ
transport ran raw `curl_ws_*` on a dedicated `std::thread`; here the handshake
is driven by short non-blocking `curl_multi_perform` rounds between
`async::sleep_for` ticks, and `receive` / `send` use connect-only's
non-blocking `curl_ws_recv` / `curl_ws_send` (CURLE_AGAIN) and suspend on asio
socket readiness — a `dup` of curl's active socket wrapped in
`asio::posix::stream_descriptor`, raced against the caller's deadline. An idle
gateway connection therefore costs no CPU-pool thread, which matters under the
default `RuntimeConfig{.cpu_workers = 1}` where a blocking receive loop would
starve every other blocking task.

Two correctness bugs in the in-progress code were found and fixed under test:

1. **Connect lost the socket.** In connect-only mode the live connection is
   parked in the *multi* handle's connection pool, and `curl_ws_recv/send`
   only relocate it while the easy handle stays attached to a live multi. The
   original code removed the easy handle from the multi (and destroyed the
   local multi) right after the handshake, so `CURLINFO_ACTIVESOCKET` came back
   empty and every successful-connect test failed. Fix: the `Impl` now owns the
   multi handle and keeps the easy handle attached for the connection's whole
   lifetime, querying the socket while attached; `CurlMulti` gained move
   semantics for the ownership transfer.
2. **Close frame was dropped on teardown.** `close()` sent the close frame and
   returned immediately; the subsequent `curl_easy_cleanup` on destruction tore
   the connection down before the loopback peer read the bytes. Fix: `close()`
   now completes the RFC 6455 closing handshake — after sending, it drains
   inbound frames until the peer's close echo / EOF (bounded by 5 s so an
   unresponsive peer cannot hang teardown), guaranteeing delivery.

### Files Modified

- `include/oran/http/websocket.hpp` (new)
- `src/oran-http/websocket.cpp` (new)
- `src/oran-http/_impl/curl_common.hpp` (new — shared curl RAII / error xlat)
- `src/oran-http/client.cpp` (now consumes `_impl/curl_common.hpp`; ~150 lines
  of duplicated curl wrappers removed)
- `include/oran/http.hpp` (umbrella header now includes websocket.hpp)
- `tests/http/test_websocket.cpp` (new)
- `tests/test-helpers/ws_test_server.hpp` (new scripted loopback ws server)
- `src/oran-bootstrap/bootstrap.cpp` (slice tag → slice 231)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumps the snapshot to slice 231, refreshes the `oran-http`
  surface count (3/21 → 27/148; the old number was stale), splits milestone 2b
  into 2b-i (shipped) / 2b-ii (next: the `GatewaySession` driver).
- `docs/ROADMAP.md` — moves the Channels row frontier to the shipped WebSocket
  boundary and names the gateway driver as the next step.
- `docs/ARCHITECTURE.md` — extends the `oran-http` inventory row with the
  WebSocket boundary and the shared `_impl/curl_common.hpp`.
- `docs/design-docs/channel-abstraction.md` — records milestone 2b-i under
  "QQ Adapter Migration".
- `docs/product-specs/0003-multi-platform-channels.md` — adds slice 231 to
  "Shipped So Far".
- `docs/QUALITY_SCORE.md` — refreshes the `oran-http` test count.
- `docs/exec-plans/active/2026-06-10-channel-qq-port.md` — splits milestone 2b
  into 2b-i/2b-ii, checks off 2b-i, and records the slice-231 decision log.
- `docs/releases/feature-release-notes.md` — adds the slice 231 release note.

### Validation

- Commands run:
  - `xmake f -m release && xmake build oran-http` + `xmake -j$(nproc)` (full
    build clean)
  - `xmake build test-http && xmake run test-http` — **27 cases / 148
    assertions** (12 new websocket cases / 88 assertions); websocket suite run
    10× with no flakiness.
  - `xmake f -m debug --sanitizers=y && xmake build test-http` →
    `test-http "[websocket]"` clean under ASan/UBSan (validates the `dup`'d fd,
    the asio descriptor, and curl handle ownership).
  - `make ci`
- Tests added/changed:
  - New `test_websocket.cpp`: request-shape validation (empty/non-ws/zero
    timeout), handshake + text round-trip (asserts the `Authorization` header
    and recorded client frame), receive-timeout-then-message, peer close
    surfaced once then conflict on reuse, fragmented reassembly, message larger
    than one recv chunk (70 KB), ping/pong transparency (curl auto-pong),
    binary kind, close sends code+reason then conflict on re-close,
    upgrade-rejection failure, connect-refused failure, and cancellation while
    suspended in `receive`.
- Bench impact:
  - None added. The boundary is network-bound; per
    `rules/testing-and-bench.md` ("Do not bench when ... correctness not
    speed / not a hot path") a microbench is not meaningful. The bucket-level
    A-vs-B floor (C12) is already met by `bench/http/scenarios/client.cpp`.
- Compile-budget delta:
  - One new TU (`websocket.cpp`) on the existing `oran-http` row; `client.cpp`
    shrinks by ~150 lines moved to the shared `_impl/curl_common.hpp`. No new
    third-party dependency.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
- Next slice: milestone 2b-ii — drive `qq::GatewaySession` over
  `http::WebSocket` (persistent read loop, heartbeat timer over
  `async::sleep_for`, reconnect backoff) behind `Channel::next_message()` in
  `oran-channel-qq`.

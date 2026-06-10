## [2026-06-10 13:25] | Task: channel-qq API client and token store

### Execution Context

- Agent: `Claude Code`
- Base model: `Fable 5`
- Runtime: `Claude Code CLI`
- Linked plan:
  `docs/exec-plans/active/2026-06-10-channel-qq-port.md`

### User Query

> Deeply understand the project architecture and current implementation
> progress, read all relevant documentation, then start implementing.

### Changes Overview

- Areas: new gated `oran-channel-qq` library (API client + token store),
  build wiring (`channel_qq` option, targets/tests/bench, compile budget,
  dep-graph layer), channel design/spec docs, status/roadmap/quality/release
  tracking.
- Key actions: added `qq::TokenStore` (app-access-token fetch over
  `https://bots.qq.com/app/getAppAccessToken`, refresh-ahead expiry policy,
  `expires_in` int-or-string coercion with a 60 s floor, single-flight
  refresh, `invalidate()` for the 401 path) and `qq::ApiClient`
  (`get`/`post`/`put`/`del` against `https://api.sgroup.qq.com`,
  `Authorization: QQBot <token>`, the legacy retry ladder — one token
  refresh on 401, ≤2 `retry-after`-honoring retries on 429, ≤2 exponential
  backoff retries on 502/503/504 — plus the public pure seam
  `normalize_api_response` capturing `x-tps-trace-id` / `retry-after`
  headers and the `{"code","message"}` business envelope). All waits go
  through `async::sleep_for`; errors mirror the provider transport's
  status-class mapping and carry method/path/status/trace/biz context but
  never raw bodies or secrets (C5).

### Design Intent

This is milestone 1 of the QQ-port plan: validate the platform's
request/response shapes offline before any transport or trait work.
The legacy `qq-api-client.cpp` was rewritten, not copied: libcurl handling
moved behind `oran-http::Client`, the throwing surface became
`Result<ApiResponse>`, and the legacy `std::mutex` + `std::this_thread::sleep_for`
retry waits became cancel-aware coroutine awaits. The token-refresh race the
plan calls out is handled with the `oran-io` singleflight idiom (per-waiter
`asio::steady_timer`, leader wakes by rescheduling, shared outcome snapshot)
rather than a strand, because a strand alone cannot prevent double-refresh
across `co_await` suspension points; waiters whose leader was *cancelled*
(not failed) retry as the new leader so one cancelled caller doesn't poison
the rest. Bodies cross the public API as serialized JSON strings so the
headers stay C6-clean; nlohmann lives only in the `.cpp` files.
`option("channel_qq")` defaults **off** per the plan's decision log — an
unvalidated network adapter must not be in default builds — and the gated
target declares only the deps milestone 1 consumes (`oran-core`,
`oran-async`, `oran-http`); `oran-channel` joins when the trait adapter
lands. The test bucket's `ScriptedHttpServer` pins
`NO_PROXY=127.0.0.1,localhost` for its lifetime because curl ignores
wildcard `no_proxy` entries like `127.*` and loopback tests must bypass any
configured proxy (this initially failed 17 of 21 cases in this WSL
environment).

### Files Modified

- `include/oran/channel-qq.hpp`
- `include/oran/channel-qq/token_store.hpp`
- `include/oran/channel-qq/api_client.hpp`
- `src/oran-channel-qq/token_store.cpp`
- `src/oran-channel-qq/api_client.cpp`
- `tests/channel-qq/main.cpp`
- `tests/channel-qq/scripted_http_server.hpp`
- `tests/channel-qq/test_token_store.cpp`
- `tests/channel-qq/test_api_client.cpp`
- `bench/channel-qq/main.cpp`
- `bench/channel-qq/scenarios/normalize_response.cpp`
- `bench/channel-qq/README.md`
- `xmake/options.lua` / `xmake/targets.lua` / `xmake/tests.lua` / `xmake/bench.lua`
- `compile_budget.json`
- `scripts/check-deps.sh`
- `src/oran-bootstrap/bootstrap.cpp` (slice tag)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumps the snapshot to slice 229, adds the gated
  `oran-channel-qq` surface counts, points the next slice at the receive
  transport.
- `docs/ROADMAP.md` — moves the Channels row frontier to the shipped API
  client + token store and names the long-poll transport as the next step.
- `docs/ARCHITECTURE.md` — fills the `oran-channel-qq` inventory row with
  the shipped surface and current (milestone-1) dependency set.
- `docs/BUILD_SYSTEM.md` — documents the `channel_qq` option and the gated
  adapter target/test/bench wiring.
- `docs/design-docs/channel-abstraction.md` — records the shipped milestone
  and the default-off option decision.
- `docs/product-specs/0003-multi-platform-channels.md` — adds slice 229 to
  "Shipped So Far".
- `docs/QUALITY_SCORE.md` — adds the channel-qq test/bench buckets and
  refreshes the Channels row.
- `docs/rules/libraries.md` — adds `channel-qq` to the nlohmann_json
  consumer list.
- `docs/exec-plans/active/2026-06-10-channel-qq-port.md` — checks off
  milestone 1 and records the decision log.
- `docs/releases/feature-release-notes.md` — adds the slice 229 release
  note.

### Validation

- Commands run:
  - `xmake f -m release --channel_qq=y && xmake build oran-channel-qq`
  - `xmake build test-channel-qq && xmake run test-channel-qq`
  - `xmake build bench-channel-qq && xmake run bench-channel-qq`
  - `xmake -j$(nproc)` (full build, option on)
  - `xmake test` (all 19 buckets passed, including `test-channel-qq`)
  - `xmake f --channel_qq=n && xmake -j$(nproc)` (zero channel-qq targets
    configured; verified via `xmake show -l targets`)
  - `xmake run orangutan -- --help` (reports `2.0.0-slice229`)
  - `make ci`
- Tests added/changed:
  - New `test-channel-qq` bucket: 21 cases / 129 assertions — token
    fetch/cache/refresh-ahead/TTL-floor, single-flight (one token request
    under two concurrent callers against a delayed mock response), shared
    failed-outcome propagation, invalidate-refetch, 401-refresh-retry,
    429 `retry-after`, 502 backoff, business-envelope and status-class error
    mapping, absolute-URL bypass, two cancellation cases (mid-refresh and
    mid-backoff), and pure `normalize_api_response` coverage.
- Bench impact:
  - New `bench-channel-qq` `normalize_response` A-vs-B: empty-body
    normalization ~97 ns vs. JSON business-envelope normalization ~1.12 µs
    per response locally (~11× — the empty-body guard and the
    parse-only-when-body-present ordering are justified).
- Compile-budget delta:
  - One new gated library (two TUs) with an `oran-channel-qq` row at the
    `oran-channel-*` category budget (1.2 s / 2.5 s / 3.0 s); disabled
    builds configure zero adapter targets. No new third-party dependency
    (`nlohmann_json` and `asio` were already in the package set).

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`

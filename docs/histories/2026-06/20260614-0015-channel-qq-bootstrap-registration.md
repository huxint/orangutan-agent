## [2026-06-14 00:15] | Task: channel-qq bootstrap registration

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: Codex CLI/API, xmake release builds
- Linked plan: `docs/exec-plans/active/2026-06-10-channel-qq-port.md`

### User Query

Continue the long-running slice workflow with an effectively unbounded budget:
read the development docs first, understand real project progress, choose the
highest-value current slice instead of inventing a status-only "next slice",
then complete implementation, validation, docs/history sync, and a detailed
Conventional Commit.

### Changes Overview

- Areas: `oran-config`, `oran-bootstrap`, gated `oran-channel-qq` integration,
  xmake feature wiring, channel docs, release/status docs.
- Key actions:
  - Extended `config.channels[]` for QQ metadata:
    `qq_app_id_env`, `qq_client_secret_env`, `qq_token_url`,
    `qq_api_base_url`, and `qq_gateway_url`.
  - Kept config secret-safe: the first two fields are environment-variable
    names, never credential values, and QQ entries require the credential env
    names plus a gateway URL.
  - Wired enabled `--channel_qq=y` bootstrap builds to link
    `oran-channel-qq`, define `ORAN_ENABLE_CHANNEL_QQ`, resolve QQ credential
    env vars at registration time, and register QQ entries as generic
    `channel::Channel` adapters.
  - Added a private bootstrap-owned wrapper so `http::Client`,
    `qq::TokenStore`, `qq::ApiClient`, `qq::GatewayTransport`, and
    `qq::QqChannel` lifetimes stay together after registration.
  - Preserved default `--channel_qq=n` behavior: QQ entries are skipped and
    reported without compiling or linking the optional adapter target.

### Design Intent

This completes QQ-port milestone 3c without changing runtime ownership. The
registration seam remains construction-only: it does not start adapters, open a
gateway receive loop, or perform network I/O during registration beyond reading
the configured env vars. The public `qq::QqChannel` constructor stays
borrow-based because tests and future direct owners can still assemble it
explicitly; bootstrap solves its own lifetime problem with a private owning
wrapper instead of broadening the adapter API.

Gateway discovery is deliberately not added here. The slice requires
`qq_gateway_url` because discovering `/gateway/bot` is live async network
behavior, and the plan keeps that in milestone 4 with the full round-trip
acceptance path.

### Files Modified

- `include/oran/config/config.hpp`
- `src/oran-config/config.cpp`
- `tests/config/test_config.cpp`
- `include/oran/bootstrap/channel_ingress.hpp`
- `src/oran-bootstrap/channel_ingress.cpp`
- `tests/bootstrap/test_channel_ingress.cpp`
- `include/oran/channel-qq/channel.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `xmake/targets.lua`
- `config.example.json`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped the slice to 235, pointed at this history, and
  refreshed the channel/bootstrap/config test counts.
- `docs/ROADMAP.md` - moved the Channels frontier to QQ milestone 3c and kept
  milestone 4 as the next concrete slice.
- `docs/exec-plans/active/2026-06-10-channel-qq-port.md` - marked milestone 3c
  complete and recorded the registration/ownership decision.
- `docs/design-docs/channel-abstraction.md` - documented QQ config-backed
  bootstrap registration and the remaining round-trip boundary.
- `docs/product-specs/0003-multi-platform-channels.md` - added the shipped
  slice-235 state while leaving the QQ round-trip acceptance criterion open.
- `docs/design-docs/bootstrap-runtime.md` - described construction-only QQ
  registration and default-build skip behavior.
- `docs/design-docs/secrets-and-state.md` - documented QQ env-name config and
  the bootstrap credential-read boundary.
- `docs/RELIABILITY.md` - documented required QQ env vars for enabled QQ
  channel registration.
- `docs/SECURITY.md` - recorded the non-secret config and missing-credential
  failure posture for QQ bootstrap registration.
- `docs/BUILD_SYSTEM.md` - documented the enabled-build bootstrap dependency
  and compile define.
- `docs/ARCHITECTURE.md` - updated the `oran-channel-qq` and `oran-bootstrap`
  inventory rows for slice 235.
- `docs/QUALITY_SCORE.md` - refreshed the config/bootstrap/channel-qq coverage
  and channel-quality notes.
- `docs/releases/feature-release-notes.md` - added the user-visible slice 235
  release note.
- `config.example.json` - added a QQ channel example with env names and
  endpoints only.

### Validation

- Commands run:
  - `xmake f -m release --channel_qq=y`
  - `xmake build test-config`
  - `xmake build test-bootstrap`
  - `xmake build test-channel`
  - `xmake build test-channel-qq`
  - `build/linux/x86_64/release/test-config`
  - `build/linux/x86_64/release/test-bootstrap`
  - `build/linux/x86_64/release/test-channel`
  - `build/linux/x86_64/release/test-channel-qq`
  - `xmake build bench-channel-qq`
  - `build/linux/x86_64/release/bench-channel-qq`
  - `xmake build bench-channel`
  - `build/linux/x86_64/release/bench-channel`
  - `xmake f -m release --channel_qq=n`
  - `xmake build test-bootstrap`
  - `build/linux/x86_64/release/test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
- Tests added/changed:
  - `test-config`: QQ channel metadata extraction plus missing/empty QQ field
    rejection; enabled result: 55 cases / 519 assertions.
  - `test-bootstrap`: default-build QQ skip/report coverage; enabled-build QQ
    registration and missing credential env failure coverage. Default result:
    146 cases / 1312 assertions. Enabled result: 147 cases / 1319 assertions.
  - `test-channel`: unchanged bucket still passed, 25 cases / 191 assertions.
  - `test-channel-qq`: unchanged bucket still passed under `--channel_qq=y`,
    55 cases / 344 assertions.
- Bench impact:
  - `bench-channel-qq` passed; local medians were ~83 ns for empty-body
    normalization, ~894 ns for JSON business-envelope normalization, ~202 ns
    for heartbeat-ack gateway consume, and ~1.42 us for dispatch consume.
    Nanobench still reports the heartbeat-ack row as unstable due to low
    iteration count, not as a failing result.
  - `bench-channel` passed; local medians were ~451 ns direct append,
    ~10.67 us manager receive-one, ~3.90 us direct runner, and ~37.86 us
    mock-ingress dispatch.
- Compile-budget delta:
  - No budget row changed. The optional QQ target remains default-off; enabled
    builds add the expected `oran-bootstrap` dependency on `oran-channel-qq`.
  - Rebuilding default `test-bootstrap` after toggling `channel_qq` emitted the
    existing GCC `maybe-uninitialized` warning in
    `tests/bootstrap/test_automation_prompt_runner.cpp:369`, outside this
    slice, and still completed successfully.
- Command-shape note:
  - `xmake build test-config test-bootstrap test-channel test-channel-qq`
    is not accepted by this xmake version because `build` takes one target
    argument; the four build commands above were run individually.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
- Next plan step: QQ-port milestone 4, the mock-server registered-path
  round-trip plus manual real-credential smoke gate before considering
  `channel_qq` default-on.

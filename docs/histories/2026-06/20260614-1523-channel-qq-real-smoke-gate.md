## [2026-06-14 15:23] | Task: channel-qq real-smoke gate

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

- Areas: gated QQ channel bootstrap integration, manual/nightly smoke coverage,
  channel docs, reliability docs, release/status docs.
- Key actions:
  - Added a hidden Catch2 real-smoke entrypoint in `test-bootstrap` for the
    registered QQ path. It is tagged `[.]` / `[manual]`, requires
    `ORAN_TEST_QQ_REAL_SMOKE=1`, and returns a no-op success when required QQ
    smoke environment variables are absent.
  - The smoke keeps provider behavior deterministic with the existing
    `RecordingProvider`, while real QQ credentials, gateway URL, token
    endpoint, API base, and incoming operator message exercise the actual QQ
    adapter path: bootstrap registration, `ChannelManager::start_all`,
    `receive_one`, routed prompt dispatch, trace/audit observation, passive QQ
    reply send, and shutdown.
  - Bumped the binary slice tag to `2.0.0-slice237`.

### Design Intent

Slice 236 proved the registered QQ path with loopback HTTP/WebSocket fixtures,
but there was still no repository-local command a nightly job or operator could
run to exercise the same boundary against Tencent's real platform. This slice
turns milestone 4b from an intention into an executable gate while keeping
ordinary CI secret-free and network-free.

This does not claim the real smoke has passed. The current environment has no
QQ real-smoke variables set, so the new test was validated for compile/default
hidden/no-op behavior only. The remaining QQ-port decision is to run this
hidden test with real credentials and record the result before considering
`channel_qq` default-on.

### Files Modified

- `tests/bootstrap/test_channel_ingress.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped the slice to 237 and recorded the real-smoke gate
  entrypoint while keeping the credentialed run as the next concrete step.
- `docs/ROADMAP.md` - moved the Channels frontier to the executable 4b gate
  and kept the credentialed run/default-on decision as next.
- `docs/exec-plans/active/2026-06-10-channel-qq-port.md` - split 4b into the
  executable gate and the still-open credentialed run.
- `docs/design-docs/channel-abstraction.md` - documented the hidden real-smoke
  entrypoint and its default-off boundary.
- `docs/product-specs/0003-multi-platform-channels.md` - added the slice-237
  shipped state while leaving QQ acceptance criterion 2 open.
- `docs/RELIABILITY.md` - documented the real-smoke environment variables and
  run command without exposing secret values.
- `docs/QUALITY_SCORE.md` - refreshed the channel frontier.
- `docs/releases/feature-release-notes.md` - added the slice 237 release note.

### Validation

- Commands run:
  - `git diff --check` — passed.
  - `xmake f -m release --channel_qq=y --ccache=n` — passed.
  - `xmake clean test-bootstrap` — passed.
  - `xmake build test-bootstrap` — passed. GCC still emits the existing
    `tests/bootstrap/test_automation_prompt_runner.cpp:369`
    `-Wmaybe-uninitialized` warning; no new warning is tied to this slice.
  - `build/linux/x86_64/release/test-bootstrap "[channel-qq]" --reporter=console --verbosity=normal`
    — passed, 4 cases / 49 assertions.
  - `build/linux/x86_64/release/test-bootstrap "registered QQ channels real-smoke one operator message through the routed prompt path" --reporter=console --verbosity=normal`
    — passed, 1 case / 1 assertion. This proves the explicit no-credential
    no-op command exits successfully; it is not a real QQ pass.
  - `build/linux/x86_64/release/test-bootstrap --reporter=console --verbosity=normal`
    under `--channel_qq=y` — passed, 148 cases / 1352 assertions.
  - `xmake build test-channel-qq` — passed.
  - `build/linux/x86_64/release/test-channel-qq --reporter=console --verbosity=normal`
    — passed, 55 cases / 344 assertions.
  - `xmake f -m release --channel_qq=n --ccache=n` — passed.
  - `xmake clean test-bootstrap` — passed.
  - `xmake build test-bootstrap` — passed under the default build.
  - `build/linux/x86_64/release/test-bootstrap --reporter=console --verbosity=normal`
    under the default build — passed, 146 cases / 1312 assertions.
  - `xmake build orangutan` — passed.
  - `xmake run orangutan -- --help` — passed and reported
    `orangutan v2.0.0-slice237`.
  - `make ci` — passed.
- Environment check: all `ORAN_TEST_QQ_*` smoke variables were unset locally
  (`REAL_SMOKE`, app id, client secret, gateway URL, optional endpoints,
  channel id, agent key, reply text, timeout), so no credentialed real-network
  QQ pass is claimed.
- Tests added/changed: one hidden `[.][manual][bootstrap][channel_ingress][channel-qq][async]`
  `test-bootstrap` case plus QQ-only env/config helpers.
- Bench impact: none; this is an operator/nightly correctness gate, not a hot
  runtime path.
- Compile-budget delta: default builds do not include the QQ smoke helpers;
  the extra `<array>`, `<charconv>`, and `nlohmann/json.hpp` includes are
  behind `ORAN_ENABLE_CHANNEL_QQ` in the test TU only.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`
- Next plan step: run the hidden real QQ smoke test with real credentials and
  record the result before considering `channel_qq` default-on.

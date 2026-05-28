## [2026-05-28 22:10] | Task: stabilize localhost HTTP tests under proxy env

### Execution Context

- Agent: Codex GPT-5
- Base model: GPT-5
- Runtime: Codex CLI
- Linked plan: none; diagnostic/test-stability fix outside the active scheduler plan.

### User Query

> `xmake test` builds successfully but appears to hang after printing every
> `running.test test-*/default` line.

### Changes Overview

- Areas: HTTP/bootstrap test harnesses.
- Key actions:
  - Force the localhost HTTP tests to bypass user-global proxy settings by
    setting `NO_PROXY` and `no_proxy` to `127.0.0.1,localhost` inside the
    affected test cases.
  - Harden the three local `OneShotHttpServer` fixtures so teardown wakes the
    blocking synchronous `accept()` path even when the expected client request
    never arrives.

### Design Intent

The failing path was environmental but the hang was a test bug. The developer
environment carried `HTTP_PROXY` / `http_proxy` pointing at a localhost proxy and
used `NO_PROXY=127.*,...`; libcurl does not reliably treat that spelling as a
match for `127.0.0.1`, so the test client could miss the in-process one-shot
server. When that happened, the fixture destructor waited for the server
`std::jthread` while the worker was still blocked in synchronous `accept()`.

The fix keeps production proxy behavior untouched and makes only the tests
deterministic: localhost fixture traffic opts out of proxies explicitly, and the
fixture teardown has a bounded wake path for failure cases.

### Files Modified

- `tests/http/test_client.cpp`
- `tests/bootstrap/test_provider_backend.cpp`
- `tests/bootstrap/test_bootstrap.cpp`
- `docs/STATUS.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/histories/2026-05/20260528-2210-localhost-http-test-proxy.md` — records
  the proxy-sensitive localhost test hang and the fixture hardening.
- `docs/STATUS.md` — points `Last completed history` at this maintenance entry
  while leaving the active scheduler slice unchanged.
- No external doc invalidation; the change is internal to test fixtures and does
  not affect a documented production contract.

### Validation

- Commands run:
  - `timeout 20s xmake run -y test-http`
  - `timeout 60s xmake run -y test-bootstrap`
  - `timeout 60s xmake test`
  - `make ci`
  - `git diff --check`
  - `timeout 60s xmake test` (final full-suite re-run: 14 / 14 targets passed)
- Tests added/changed: hardened existing localhost HTTP fixture tests; no new
  cases or assertions.
- Bench impact: none.
- Compile-budget delta: no production translation units changed.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: none; not user-visible production behavior.

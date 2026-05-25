## [2026-05-26 04:26] | Task: `oran-http` body client

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: API workspace agent
- Linked plan: `docs/exec-plans/active/2026-05-26-provider-adapter-v1.md`

### User Query

> Continue implementing the project after deeply reading the docs; keep one version
> per commit and use detailed commit messages.

### Changes Overview

- Areas: `oran-http`, xmake build/test/bench registration, provider handoff docs,
  slice version.
- Key actions: added `<oran/http.hpp>` and `<oran/http/client.hpp>`, implemented a
  pimpl-backed libcurl body-response `http::Client`, registered `oran-http`,
  `test-http`, and `bench-http`, declared the system `libcurl >=8.11.0`
  dependency, documented/allowed the intentional `oran-http -> oran-async`
  platform sibling edge, and bumped the binary slice tag to `2.0.0-slice110`.

### Design Intent

Slice 109 proved provider protocol factories over an injected
`provider::ProtocolTransport`, but a real platform transport still needed to exist
before bootstrap could construct concrete adapters. This slice keeps that transport
in `oran-http` rather than `oran-provider`, preserving the provider boundary and
keeping curl handles private to a `.cpp`. The client takes a caller-owned blocking
executor so future bootstrap wiring can run libcurl work on
`async::Runtime::cpu_executor()` instead of blocking the main coroutine executor.
Streaming/SSE and the actual `ProtocolTransport` adapter remain separate slices.

### Files Modified

- `include/oran/http.hpp`
- `include/oran/http/client.hpp`
- `src/oran-http/client.cpp`
- `tests/http/main.cpp`
- `tests/http/test_client.cpp`
- `bench/http/README.md`
- `bench/http/main.cpp`
- `bench/http/scenarios/client.cpp`
- `xmake/packages.lua`
- `xmake/targets.lua`
- `xmake/tests.lua`
- `xmake/bench.lua`
- `scripts/check-deps.sh`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 110, pointed at this history, and recorded
  the focused `test-http` count.
- `docs/ARCHITECTURE.md` — updated the `oran-http` inventory row.
- `docs/BUILD_SYSTEM.md` — documented the system libcurl package and target entry.
- `docs/QUALITY_SCORE.md` — added `oran-http` test/bench status and clarified
  remaining provider/bootstrap handoff work.
- `docs/design-docs/api-portability.md` and `docs/design-docs/bootstrap-runtime.md`
  — recorded that HTTP body transport exists, while adapter construction is still
  downstream.
- `docs/product-specs/0001-core-react-loop.md` and
  `docs/product-specs/0017-fake-provider-first-agent-loop.md` — narrowed the
  remaining work from "no concrete HTTP client" to "bind the client into provider
  adapter/bootstrap handoff".
- `docs/exec-plans/active/2026-05-26-provider-adapter-v1.md` — marked the body
  client complete and split out bootstrap adapter construction.
- `docs/releases/feature-release-notes.md` — added the slice-110 release note.
- `docs/rules/libraries.md`, `include/README.md`, `tests/README.md`, and
  `bench/README.md` — reflected the new library, dependency, test bucket, and bench
  bucket.
- `docs/design-docs/module-boundaries.md` and `scripts/check-deps.sh` — recorded
  `oran-http -> oran-async` as an intentional platform-layer sibling dependency
  because callers provide the executor that owns blocking transport work.

### Validation

- Commands run:
  - `xmake run test-http`
  - `xmake run bench-http`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `make ci`
- Tests added/changed:
  - `tests/http/test_client.cpp` covers a localhost POST round trip, request-shape
    validation, and parent cancellation before dispatch.
- Bench impact:
  - Added and ran `bench-http` with client construction vs. invalid-request
    validation. The invalid-request row lowers its local minimum iteration count so
    the coroutine validation path stays bounded while the cheap constructor baseline
    keeps a higher iteration count for stability.
- Compile-budget delta:
  - Not measured in this slice; `oran-http` has one implementation TU and keeps
    libcurl private to the `.cpp`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`.

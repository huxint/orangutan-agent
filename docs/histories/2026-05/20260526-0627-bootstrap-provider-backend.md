## [2026-05-26 06:27] | Task: bootstrap HTTP provider backend

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: API workspace agent
- Linked plan: `docs/exec-plans/active/2026-05-26-provider-adapter-v1.md`

### User Query

> Continue implementing the project after deeply reading the docs; keep one version
> per commit and use detailed commit messages.

### Changes Overview

- Areas: `oran-bootstrap`, provider adapter handoff, HTTP-backed provider construction
  tests, docs/status.
- Key actions: added `HttpProviderBackendOptions` and movable
  `HttpProviderBackend`, implemented the bootstrap-owned adapter from
  `http::Client` to `provider::ProtocolTransport`, registered built-in
  Anthropic Messages and OpenAI Responses protocol factories, resolved
  configured API-key environment variables through the existing credential
  boundary, and bumped the binary slice tag to `2.0.0-slice111`.

### Design Intent

Slice 110 supplied a concrete platform HTTP client, while slice 109 had already proven
provider protocol factories over an injected transport. This slice connects those
pieces in `oran-bootstrap`, not `oran-provider`, so libcurl ownership stays in
`oran-http` and provider protocol code remains transport-injected and offline-testable.
The seam is explicit: tests and future runner owners can build a real backend, but
ordinary `bootstrap::run` still stays on the no-runner CLI path until the final
`cli::run_async` handoff slice intentionally opts into credentials and network I/O.

### Files Modified

- `include/oran/bootstrap.hpp`
- `include/oran/bootstrap/provider_backend.hpp`
- `include/oran/provider/protocol_transport.hpp`
- `include/oran/provider/system.hpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `src/oran-bootstrap/provider_backend.cpp`
- `tests/bootstrap/test_provider_backend.cpp`
- `xmake/targets.lua`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped to slice 111, pointed at this history, and recorded the
  focused `test-bootstrap` count.
- `docs/ARCHITECTURE.md` - updated the `oran-bootstrap` / `oran-provider` inventory
  and top-level status text for the new HTTP provider backend seam.
- `docs/BUILD_SYSTEM.md` - documented the new `oran-bootstrap -> oran-http`
  dependency and clarified that ordinary `bootstrap::run` still does not opt in.
- `docs/QUALITY_SCORE.md` - refreshed bootstrap/provider status and the
  `test-bootstrap` count.
- `docs/design-docs/api-portability.md`, `docs/design-docs/bootstrap-runtime.md`,
  `docs/design-docs/agent-platform.md`, and `docs/design-docs/tool-runtime.md` -
  recorded that `http::Client` is now bound into `ProtocolTransport` through
  bootstrap while ordinary binary handoff remains downstream.
- `docs/product-specs/0001-core-react-loop.md` and
  `docs/product-specs/0014-structured-tool-output.md` - narrowed the remaining
  provider handoff work to ordinary binary async wiring and later streaming.
- `docs/exec-plans/active/2026-05-26-provider-adapter-v1.md` - marked slice 111
  complete and left the binary handoff as the final milestone.
- `docs/releases/feature-release-notes.md` - added the slice-111 release note.
- `include/README.md` - kept the public-header inventory current.

### Validation

- Commands run:
  - `xmake run test-http`
  - `xmake run test-provider`
  - `xmake run test-agent`
  - `xmake run test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `make ci`
  - `git diff --check`
- Tests added/changed:
  - `tests/bootstrap/test_provider_backend.cpp` covers a localhost Anthropic
    Messages round trip through the libcurl client and a missing credential
    construction error whose context contains only non-secret fields.
- Bench impact:
  - No new bench. This is one-time backend construction plus an injected transport
    adapter; `oran-http` and `oran-provider` already own the relevant microbench
    surfaces.
- Compile-budget delta:
  - Not measured in this slice; the new public header is pimpl-based and keeps curl
    and provider construction details out of consumers.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`.

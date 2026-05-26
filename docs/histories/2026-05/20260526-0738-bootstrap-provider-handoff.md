## [2026-05-26 07:38] | Task: bootstrap provider handoff

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: API workspace agent
- Linked plan: `docs/exec-plans/completed/2026-05-26-provider-adapter-v1.md`

### User Query

> Continue implementing the project after deeply reading the docs; keep one version
> per commit, use detailed commit messages, and commit the current slice first.

### Changes Overview

- Areas: `oran-bootstrap`, ordinary CLI provider handoff, HTTP-backed provider
  runner tests, docs/status.
- Key actions: switched configured-route `bootstrap::run` from the deterministic
  no-runner shell to `cli::run_async` with `AgentPromptRunner`, built
  `HttpProviderBackend` on the process runtime's CPU executor, passed
  `runtime.request_timeout_ms` to provider body requests, preserved the no-route
  built-in defaults path, and bumped the binary slice tag to `2.0.0-slice112`.

### Design Intent

The provider adapter v1 plan had already landed route/profile metadata, explicit
credential resolution, protocol JSON mappers, injected protocol factories, a concrete
HTTP client, and bootstrap's `HttpProviderBackend` seam. This slice intentionally
crosses the final ordinary-binary boundary only when config declares a `default`
provider route: configured prompts now use the same `AgentPromptRunner` path proven
by tests, while fresh checkouts without routes still run without provider credentials
or network traffic.

### Files Modified

- `src/oran-bootstrap/bootstrap.cpp`
- `include/oran/bootstrap/prompt_runner.hpp`
- `tests/bootstrap/test_bootstrap.cpp`
- `README.md`
- `docs/exec-plans/completed/2026-05-26-provider-adapter-v1.md`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - bumped to slice 112, pointed at this history, and marked the
  provider adapter v1 plan complete.
- `docs/QUALITY_SCORE.md` - refreshed bootstrap, provider, agent, CLI, and test-count
  status for configured-route ordinary binary handoff.
- `docs/releases/feature-release-notes.md` - added the slice-112 user-visible note.
- `docs/ARCHITECTURE.md`, `docs/BUILD_SYSTEM.md`, `docs/RELIABILITY.md`,
  `docs/SECURITY.md`, `docs/design-docs/agent-platform.md`,
  `docs/design-docs/api-portability.md`, `docs/design-docs/bootstrap-runtime.md`,
  `docs/design-docs/cli-runtime.md`, and `docs/design-docs/tool-runtime.md` -
  recorded that configured-route startup now builds the HTTP-backed provider
  backend and starts `agent::Loop`.
- `docs/product-specs/0001-core-react-loop.md`,
  `docs/product-specs/0014-structured-tool-output.md`, and
  `docs/product-specs/0017-fake-provider-first-agent-loop.md` - moved ordinary
  binary handoff out of future work and left SSE/streaming follow-ups downstream.
- `README.md` - updated binary smoke guidance so provider-backed prompts mention
  the required API-key environment variable.

### Validation

- Commands run:
  - `xmake run test-bootstrap`
  - `git diff --check`
- Tests added/changed:
  - `tests/bootstrap/test_bootstrap.cpp` now covers a localhost Anthropic Messages
    round trip through ordinary configured-route `bootstrap::run` and verifies a
    missing configured API key fails as `ErrorKind::auth` before CLI async execution.
- Bench impact:
  - No new bench. This path is one-time process startup plus the existing provider
    execution and HTTP body-client surfaces.
- Compile-budget delta:
  - Not measured in this slice; the implementation uses existing bootstrap/provider
    public seams and does not add a new dependency or heavy public include.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md`.

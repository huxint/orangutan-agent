## [2026-05-25 06:22] | Task: Provider execution runtime

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none — this is a focused provider prework slice under `docs/design-docs/api-portability.md` and spec 0017.

### User Query

> Continue iterating the project after reading the documentation, keep one coherent version per commit, keep docs in sync, and do not implement before understanding the requirements.

### Changes Overview

- Areas: `oran-provider`, provider docs, bootstrap slice tag.
- Key actions: added `<oran/provider/execution.hpp>` and `provider::execution::Runtime`, exported it from `<oran/provider.hpp>`, retried retryable provider errors per target, tried route fallbacks after primary exhaustion, suppressed retry/fallback after visible stream output, preserved provider-supplied `model_used`, filled missing `model_used` from the selected target model, and bumped the binary slice tag to `2.0.0-slice97`.

### Design Intent

`api-portability.md` keeps protocol adapters deliberately small: adapters translate the wire format, while retry, fallback, and later cost/hook policy live in the execution layer. This slice makes that boundary concrete without changing real adapters or the agent loop by implementing execution as a `provider::System` decorator over any backend system. Each backend call receives a single-target route, so future Anthropic/OpenAI adapters do not have to duplicate route fallback logic. Attempts that have already emitted stream output are treated as committed for retry purposes, because replaying them would duplicate bytes already rendered by terminal/UI callers.

### Files Modified

- `include/oran/provider/execution.hpp`
- `include/oran/provider.hpp`
- `src/oran-provider/execution.cpp`
- `tests/provider/test_execution.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR

- `docs/STATUS.md` — moved the project snapshot to slice 97 and recorded the focused `test-provider` result.
- `docs/ARCHITECTURE.md` — updated the provider inventory and remaining downstream work.
- `docs/QUALITY_SCORE.md` — refreshed provider/test counts and next steps.
- `docs/design-docs/api-portability.md` — marked retry/fallback execution as shipped and corrected the documented `model_used` type.
- `docs/design-docs/agent-platform.md` — noted that execution runtime now exists but still needs loop/binary wiring.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — updated provider retry/fallback status and remaining handoff boundary.
- `docs/releases/feature-release-notes.md` — added the slice 97 release note.

### Validation

- Commands run:
  - `xmake run test-provider`
  - `xmake run test-bootstrap`
  - `xmake run orangutan -- --help`
  - `git diff --check`
  - `make ci`
- Tests added/changed:
  - Added offline execution coverage for same-target retry, fallback success, provider-supplied `model_used`, non-retryable stop, stream-output retry suppression, zero-attempt validation, and parent cancellation during retry backoff.
- Bench impact:
  - None. The slice chooses the straightforward sequential retry/fallback policy already required by the design doc; no competing implementation needed an A/B bench.
- Compile-budget delta:
  - Minimal. The new provider execution TU uses existing async/core/provider headers; no heavy public include or third-party dependency was added.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: `docs/releases/feature-release-notes.md` row `provider-execution-runtime`.

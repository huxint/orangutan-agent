## [2026-05-25 05:24] | Task: CLI operator prompt sink for permission asks

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none — this was a focused spec-0015 follow-up after the slice-94 direct-dispatch ask bridge.

### User Query

> Continue iterating the project after reading the documentation, keep one coherent version per commit, keep docs in sync, and do not implement before understanding the requirements.

### Changes Overview

- Areas: `oran-cli`, `oran-hook`, `oran-tool` docs, build metadata, spec 0015.
- Key actions: added `cli::OperatorPromptSink`, exported it through `<oran/cli.hpp>`, made `oran-cli` depend on `oran-async` and `oran-hook`, bumped the binary slice tag to `2.0.0-slice95`, and covered scripted approval/denial behavior in `test-cli`.

### Design Intent

Slice 94 made direct dispatch publish blocking `permission_ask_rendered` events and consume proceed/veto decisions through the approval broker. This slice keeps rendering out of `oran-tool` and `oran-agent` by putting the first concrete terminal renderer in `oran-cli`, matching spec 0015's rule that approval rendering is just another blocking hook sink. The sink returns approval identity through the bus trace so all-proceed publishes can still carry `HookDecision{kind=proceed}` as the final decision while preserving the operator reason for the dispatch bridge.

Interactive input uses an asio `posix::stream_descriptor` over a duplicated stdin fd on the current coroutine executor. Tests and noninteractive callers can use `scripted_answers`, which keeps the sink deterministic without special test-only subclasses.

### Files Modified

- `include/oran/cli/operator_prompt_sink.hpp`
- `include/oran/cli.hpp`
- `src/oran-cli/operator_prompt_sink.cpp`
- `tests/cli/test_cli.cpp`
- `xmake/targets.lua`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR

- `docs/STATUS.md` — moved the project snapshot to slice 95, refreshed latest test counts, and removed the closed hook tech-debt row from the open-debt summary.
- `docs/ARCHITECTURE.md` — documented the new `oran-cli` sink and dependency shift.
- `docs/BUILD_SYSTEM.md` — mirrored the `oran-cli` dependency change and why it exists.
- `docs/QUALITY_SCORE.md` — updated test counts and area summaries for CLI/hooks/permissions/tooling.
- `docs/design-docs/cli-runtime.md` — added the `OperatorPromptSink` public API and approval prompt semantics.
- `docs/design-docs/permissions-and-hooks.md` — marked the user-visible `permission_ask_rendered` sink as shipped.
- `docs/design-docs/tool-runtime.md` — moved the remaining work from concrete sink implementation to binary binding.
- `docs/product-specs/0008-permissions.md` — reflected the terminal sink on the approval flow.
- `docs/product-specs/0012-tool-scheduler-and-state.md` — noted that binding remains a scheduler/binary handoff item.
- `docs/product-specs/0015-blocking-hook-decisions.md` — closed the concrete operator-prompt sink follow-up.
- `docs/product-specs/0017-fake-provider-first-agent-loop.md` — updated the dependency note for blocking approval rendering.
- `docs/exec-plans/tech-debt-tracker.md` — removed the now-closed 2026-05-18 hook follow-up row.
- `docs/releases/feature-release-notes.md` — added the user-visible slice 95 release note.

### Validation

- Commands run:
  - `xmake run test-cli`
  - `xmake run test-tool`
  - `xmake run test-bootstrap`
  - `make ci`
  - `xmake run orangutan -- --help`
  - `git diff --check`
- Tests added/changed:
  - Added `OperatorPromptSink` coverage for scripted approval, denial, payload identity fallback, invalid scripted answers via hook-error classification, and non-ask blocking events.
- Bench impact:
  - None. This is a terminal prompt sink, not a hot path; scripted-answer tests cover correctness.
- Compile-budget delta:
  - `oran-cli` gains one `.cpp` and public header plus `oran-async` / `oran-hook` dependencies. The interactive stdin implementation keeps asio headers out of the existing `cli.cpp` parser path.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: the 2026-05-18 hook row was closed by this slice and removed from the tracker.
- Linked release note: `docs/releases/feature-release-notes.md` row `cli-operator-prompt-sink`.

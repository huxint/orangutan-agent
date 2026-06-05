## [2026-06-05 10:03] | Task: Long-term prompt recall plumbing

### Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: local shell + xmake/GCC 16.1
- Linked plan: none; small follow-up after slice 163 gave bootstrap an owned
  `memory::longterm::Runtime`.

### User Query

> Continue iterating after the bootstrap long-term memory assembly slice, keep
> docs current, verify thoroughly, commit, and keep the next slice small.

### Changes Overview

- Areas: `oran-bootstrap`, prompt section-5 memory recall docs/status/history.
- Key actions:
  - Added `LongtermRecallOptions` on `AgentPromptRunnerOptions`.
  - Kept recall disabled by default so ordinary configured-route binary startup
    remains unchanged until config policy maps into the option explicitly.
  - When enabled, validate a positive limit, require an assembly-owned
    `memory::longterm::Runtime`, and reject exact `memory_framing` overrides.
  - At the prompt boundary, derive a `memory::longterm::Query` from the current
    user prompt plus the stable runner `scope_key`, call `Runtime::recall(...)`
    once, and feed the returned `memory::Framing` into section 5 before
    `agent::Loop` starts.
  - Added bootstrap coverage for default-off behavior, enabled recall across a
    multi-iteration provider/tool turn, and missing-runtime validation.

### Design Intent

Slice 163 made a long-term runtime available in the process assembly, but the
prompt runner still had no explicit way to consume it. This slice adds the
smallest prompt-boundary plumbing while deferring policy: selection can depend
on the current user prompt, but the rendered section-5 bytes still come only
from returned memory records. Keeping the option opt-in avoids silently changing
production prompts or creating a config contract before the query/limit policy
is designed.

### Files Modified

- `include/oran/bootstrap/prompt_runner.hpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/rules/prompt-design.md` — records section-5 recall as
  prompt-boundary state and keeps rendered bytes record-only.
- `docs/design-docs/bootstrap-runtime.md` — documents
  `AgentPromptRunnerOptions::longterm_recall` validation and runner behavior.
- `docs/design-docs/memory-system.md` — updates long-term memory status from
  future prompt recall to opt-in runner consumption.
- `docs/product-specs/0005-memory-system.md` — updates v1 scope and acceptance
  status for prompt-boundary recall.
- `docs/product-specs/0016-prompt-and-tool-catalog-cache.md` — records that
  section-5 recall now fills the existing `BuilderInputs::memory_framing` seam.
- `docs/ARCHITECTURE.md` — updates the `oran-memory` and `oran-bootstrap`
  inventory rows with the new opt-in prompt recall boundary.
- `docs/QUALITY_SCORE.md` and `docs/STATUS.md` — update slice number, latest
  history, focused test counts, and remaining follow-ups.
- `docs/exec-plans/tech-debt-tracker.md` — closes prompt-boundary recall
  rendering while leaving config/query policy, memory tools, and vector/hybrid
  search open.

### Validation

- Commands run:
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `git diff --check`
  - `make ci`
  - `xmake build orangutan`
  - `xmake run orangutan -- --help`
  - `xmake test`
- Tests added/changed:
  - `tests/bootstrap/test_prompt_runner.cpp` covers default-off recall, enabled
    recall feeding section 5 once across loop iterations, and create-time
    rejection when the assembly has no long-term runtime.
- Bench impact:
  - No new bench; this slice is prompt-boundary plumbing. The 10k-record recall
    benchmark remains open with vector/hybrid search policy.
- Compile-budget delta:
  - Not separately measured. The change adds a small public value option and
    reuses existing bootstrap/memory translation units with no new third-party
    dependencies.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: existing deep-review tracker row now leaves config/query
  recall policy, memory tools, gated sqlite-vec, and hybrid ranking as remaining
  memory work.
- Linked release note: none; opt-in embedder/runtime API with production binary
  behavior unchanged.

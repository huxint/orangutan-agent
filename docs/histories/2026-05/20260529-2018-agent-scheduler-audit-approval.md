## [2026-05-29 20:18] | Task: slice 118 — ToolScheduler approval + audit/hook fan-out correctness

### Execution Context

- Agent: Claude Opus 4.8
- Base model: claude-opus-4-8
- Runtime: Claude Code (single-session implementation)
- Linked plan: [`docs/exec-plans/active/2026-05-27-tool-scheduler-v1.md`](../exec-plans/active/2026-05-27-tool-scheduler-v1.md)
  — third of five slices (116-120). Spec
  [`docs/product-specs/0012-tool-scheduler-and-state.md`](../product-specs/0012-tool-scheduler-and-state.md)
  carries the contract.

### User Query

> Plan remaining: slices 118 (approval gating + same-row audit usage enrichment
> under parallelism), 119 (cancellation propagation + cancellation_lag), and 120
> (loop wiring + bench + config). commit first, then continue.

Slices 116 + 117 were already committed (`6b6fc1e`) with a clean working tree,
so "commit first" was already satisfied. This slice is 118: prove the per-call
audit / hook / approval invariants `tool::Registry::dispatch` guarantees for a
single call still hold when `ToolScheduler::run_batch` fans a batch out
concurrently. Closes most of AC7.

### Changes Overview

- Areas: `oran-agent` (tests only — `tests/agent/test_scheduler.cpp`).
- Key actions:
  - **No production change.** Investigation confirmed the existing dispatch
    path already preserves every per-call invariant under parallelism, so
    slice 118 is a verification slice that pins the contract with tests rather
    than altering behavior (see Design Intent).
  - Add three `[scheduler]` cases to `tests/agent/test_scheduler.cpp`:
    - **AC7 (audit + hook fan-out):** a 4-call batch mixing successful and
      failing handlers with varied latency (so completions interleave out of
      original order) records **exactly N permission-decision rows** (asserted
      against a `RecordingAuditSink`) and emits **exactly N `tool_after`
      publishes** (counted through an advisory `hook::InProcessSink` bound to
      `Event::tool_after`), including the two failing calls. The bus carries no
      blocking `tool_before` sink, so the blocking publish consults nobody and
      no `hook_publish` row is appended — the count stays N.
    - **Same-row enrichment under parallelism (slice 67):** two byte-identical
      calls (same tool, same input ⇒ same `input_hash`, same
      scope/agent/identity and a shared `parent_turn_id`) with non-zero
      `Output::usage` each enrich their **own** audit row. Asserts exactly two
      rows (enrichment updates in place, never appends), both carrying the
      turn id, and both holding a non-`"{}"` `metadata_json` with the
      serialized `usage`. If the slice-67 `previous_metadata_json` hook failed
      to keep the two enrichments paired 1:1 under parallel execution, one row
      would stay `"{}"` and this case would fail.
    - **Approval gating (ask path):** a 2-call batch of `ask` calls against a
      broker that holds a grant for one specific input — call 0 matches and is
      approved (handler runs, ordered result carries its output); call 1 carries
      an input the grant does not cover and is rejected as `permission_denied`
      at its **own** ordered index, not hidden behind the successful call. Each
      call records exactly one decision row carrying its own outcome
      (one `approved`, one `rejected`).
  - Supporting test helpers added to the anonymous namespace: `add_failing_tool`
    (sleeps then returns an infrastructure error), `add_usage_tool` (returns
    text + non-empty `Output::usage`), and `make_broker` / `grant`
    (mirroring `tests/tool/test_registry.cpp`).

### Design Intent

The central question was whether slice 118 needs a production change or is a
verification slice. The spec's per-call-invariants paragraph
(`0012` lines 104-109) says "If batched execution can run identical tool/input
pairs concurrently, the scheduler owns the stronger per-call correlation needed
to keep metadata updates attached to the correct row." I traced the actual
mechanism before writing any code:

- `Registry::dispatch` records exactly one decision row per call (`registry.cpp`)
  and always publishes `tool_after` when a bus is present, on both the success
  and failure paths — so AC7's row/publish counts are structurally N for an
  N-call batch regardless of scheduler ordering.
- Same-row usage enrichment matches on
  `event_kind + scope + agent + tool + identity + input_hash + parent_turn_id +
  previous_metadata_json` (`permission/audit.cpp`, `storage` update request).
  The `previous_metadata_json` term is the key: each `update_metadata` consumes
  exactly one not-yet-enriched row (it flips that row's metadata away from the
  matched value), so two enrichments for identical calls pair 1:1 with the two
  decision rows — at worst permuted, and identical calls have identical usage,
  so the permutation is unobservable. `parent_turn_id` (slice 79) already
  separates same-tool calls from *different* turns; within one batch every call
  shares the loop's turn id, and `previous_metadata_json` provides the
  intra-batch correlation. No new per-call correlation field is required.
- "Ask short-circuits the batch slot" is already true: the channel-as-semaphore
  permit is held across the whole `run_call`, including the broker / approval
  resolution, so the slot is not released while an `ask` is pending.

Given that, the simplest correct slice is to *verify* the invariants under the
real scheduler rather than add machinery the contract does not need (CLAUDE.md
"Simplicity First"). The same-row test is written as a cross-talk detector: it
would fail loudly if the self-correcting enrichment ever stopped pairing 1:1.
The full 100 ms cancellation guarantee + `cancellation_lag` (AC5) stay in slice
119, and loop wiring + the final AC7/AC12 closure stay in slice 120.

### Files Modified

- `tests/agent/test_scheduler.cpp` — add `<algorithm>`, `<oran/hook.hpp>`, and a
  `hook` namespace alias; add `add_failing_tool`, `add_usage_tool`, `make_broker`,
  and `grant` helpers; add the three slice-118 cases described above. No
  production source touched.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

Slice 118 changes no production behavior, interface, build, config, deps, or
layout — it adds tests that pin existing contracts — so the documented surface
is unchanged. The repo's slice-workflow docs are updated:

- `docs/STATUS.md` — bumped to slice 118; `Last completed history` points at
  this file; refreshed the `oran-agent` line in `Latest Library Surfaces`
  (40 / 10 545 → 43 / 10 590); added a slice-118 paragraph to the snapshot
  prose noting it verifies AC7 (N rows / N `tool_after`), per-call approval
  resolution, and slice-67 same-row enrichment under parallelism; refreshed the
  `Active exec-plan` line to name slice 118's AC7 progress.
- `docs/design-docs/tool-runtime.md` — promoted the "Scheduler Boundary"
  slice-118 forward-looking note into a slice-118 status note recording that
  the audit / hook fan-out and approval invariants are verified under
  parallelism, with no production change; trimmed the trailing forward-looking
  line to slices 119-120.
- `docs/exec-plans/active/2026-05-27-tool-scheduler-v1.md` — checked off the
  slice-118 audit/hook fan-out + docs bullets with a one-paragraph summary of
  the "verification, not new machinery" conclusion so the next agent does not
  re-derive it.
- No `ARCHITECTURE.md` / `QUALITY_SCORE.md` / spec `0012` edit: no new library
  surface or public API (those are slice-120 concerns per the plan; slices
  116/117 set the precedent of not annotating the spec per-slice).

### Validation

- Commands run:
  - `xmake build test-agent` — succeeded (release).
  - `./build/linux/x86_64/release/test-agent "[audit],[approval]"` — the three
    new cases pass (the filter also matches one pre-existing `[approval]` loop
    case): All tests passed (73 assertions in 4 test cases).
  - `./build/linux/x86_64/release/test-agent` — full suite: **All tests passed
    (10 590 assertions in 43 test cases).**
  - `git diff --check` — clean.
- Tests added/changed: `tests/agent/test_scheduler.cpp` — 3 new cases
  (40 → 43 cases; +45 assertions, 10 545 → 10 590).
- Bench impact: none. `bench/agent/scheduler_overhead` and
  `scheduler_audit_fanout` still land in slice 120 against the production loop
  call path.
- Compile-budget delta: none on production TUs (no production source changed).
  `test_scheduler.cpp` recompiles in ~16 s as a single Catch2 TU; test TUs are
  outside the `oran-agent` production compile budget.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none added. Slice 119's cancellation propagation +
  `cancellation_lag` and slice 120's loop wiring + bench + config stay inside the
  active arc, not the tracker.
- Linked release note: none — internal verification; the user-visible
  parallel-tool-dispatch release note lands in slice 120 with the loop wiring.

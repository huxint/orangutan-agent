## [2026-05-26 23:48] | Task: prompt runner session-state observation

### Execution Context

- Agent: Claude Opus 4.7
- Base model: claude-opus-4-7
- Runtime: Claude Code (multi-agent workflow + main-loop implementation)
- Linked plan: none — single-slice cleanup from the 2026-05-26 deep review.

### User Query

> 对项目进行 deep-review 找出实现中的不足点和缺陷/bug/性能 issue, 然后fix.
> (Do a deep review of the project, find implementation gaps and bugs/perf issues,
> then fix them. Detailed commit messages. Use workflows.)

### Changes Overview

- Areas: `oran-bootstrap` (`AgentPromptRunner`), tests/bootstrap, docs/status, docs/design-doc, tech-debt tracker.
- Key actions:
  - Wired `agent::SessionState::observe_tool_output(...)` into `AgentPromptRunner::Impl::run_prompt`
    so deferred-tool promotion is no longer dead in the production runner. After each
    successful `loop_.run_turn` call the runner walks the new transcript suffix, builds a
    `tool_use_id -> tool_name` map from assistant `ToolUseContent` blocks, and feeds each
    `ToolResultContent` back through `SessionState` as a reconstructed `tool::Output`.
    `SessionState` filters by `tool::kToolSearchName` internally; the runner counts every
    observed `tool.search` result through a new `tool_search_observations_recorded()`
    accessor for diagnostics.
  - Added an executor-presence guard in `validate_options` so a default-constructed
    `asio::any_io_executor` fails at `AgentPromptRunner::create` time instead of crashing
    later inside a tool dispatch (deep-review F19).

### Design Intent

Slice 72 introduced `agent::SessionState` as the session-owned home for prompt
promotions, and slice 101 added `AgentPromptRunner` with a `SessionState` member
plus a `promotion_snapshot(...)` call before each turn. The deep-review sweep
on 2026-05-26 noticed that `observe_tool_output(...)` is never called anywhere
in the production runner, so the snapshot is permanently empty: `tool.search`
results never reach `SessionState`, no deferred tools are ever promoted, and the
documented promotion path is dead in the binary.

This slice closes the loop without changing the loop owner's contract. The
observation happens after `run_turn` returns terminal success, so observation
errors (e.g. a buggy tool emitting malformed `data_json`) cannot back out an
already-committed audit/trace row; they are silently dropped and the runner
keeps returning the assistant text. The counter is for tests and future
diagnostics only — it is not a behavioral signal.

The executor-presence guard mirrors every other early-fail option check in
`validate_options`: a default-constructed `asio::any_io_executor` is empty,
threads through to `tool::DispatchContext::executor`, and only surfaces when a
tool tries to `asio::post` work. Failing at `create` time keeps the runner's
error shape uniform with the other config errors.

### Files Modified

- `include/oran/bootstrap/prompt_runner.hpp` — new `tool_search_observations_recorded()`
  accessor on `AgentPromptRunner`.
- `src/oran-bootstrap/prompt_runner.cpp` — added `observe_turn_results` helper,
  `tool_search_observations_` counter, `validate_options` executor check, and the
  new includes (`<unordered_map>`, `<string_view>`, `<variant>`,
  `<oran/core/content.hpp>`).
- `tests/bootstrap/test_prompt_runner.cpp` — new "rejects an empty executor at
  create time" and "feeds tool.search results back into per-session state" cases.

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 113, pointed at this history entry,
  refreshed `test-bootstrap` to 70 cases / 308 assertions, and opened a
  `review/deep-2026-05-26` row in the snapshot's tech-debt list.
- `docs/exec-plans/tech-debt-tracker.md` — added the `review/deep-2026-05-26`
  row with the remaining bullets from this session's deep review.
- `docs/design-docs/bootstrap-runtime.md` — updated the `AgentPromptRunner`
  prose so it (a) drops the now-stale "future binary handoff" wording closed by
  slice 112, and (b) names the new SessionState observation step.

### Validation

- Commands run:
  - `xmake build oran-bootstrap`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
- Tests added/changed:
  - `tests/bootstrap/test_prompt_runner.cpp` covers both the executor-validation
    guard and the SessionState observation wiring through a scripted
    `tool.search` turn.
- Bench impact:
  - No new bench. The observation walks the new transcript suffix once per
    turn; the cost is bounded by the number of tool calls in that turn and is
    dominated by existing dispatch/audit work.
- Compile-budget delta:
  - Not measured. New includes are `<unordered_map>`, `<string_view>`, `<variant>`,
    and `<oran/core/content.hpp>`; all already pulled transitively elsewhere in
    `oran-bootstrap`, so the TU's first-build cost should not change
    measurably.

### Follow-ups

- Tech-debt entries: opened `review/deep-2026-05-26` covering the remaining
  deep-review findings (`F1` trace-write error masking, `F3`/`F4`/`F23`
  oran-io singleflight race + cancel-leak, `F5` trace `route_profile` under
  fallback, `F6`/`F12` bootstrap parse_args strictness, `F9` OpenAI cancelled
  status, `F10` retry-backoff target context, `F11` OpenAI system-message
  duplication, `F13`–`F25` style + low-severity items). Land in priority order.
- Issues opened: none.
- Linked release note: none — this slice closes a regression that never
  shipped externally as documented behavior.

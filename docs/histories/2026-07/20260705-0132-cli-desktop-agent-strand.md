## [2026-07-05 01:32] | Task: CLI/desktop agent loops on a per-run strand

### Execution Context

- Agent: Claude Code
- Base model: Claude Fable 5
- Runtime: local CLI
- Linked plan: none — closes the 2026-06-20 tech-debt-tracker row under the
  Tool scheduler roadmap row ("drive the CLI agent loop on a per-agent
  strand").

### User Query

> Gain a deep understanding of the project architecture and implementation
> goals, comprehensively optimize redundancies and deficiencies, and carry out
> appropriate refactoring.

### Changes Overview

- Areas: `oran-bootstrap` (CLI + desktop wiring), `test-bootstrap`.
- Key actions: the configured-route CLI path and the `--desktop` launch now
  host the agent loop, the runner-owned `agent::ToolScheduler`, and (for
  desktop) the `ChatBridge` on one `Runtime::make_strand()` instead of the
  raw multi-worker `runtime.executor()`. `run_cli_async_on_runtime` takes the
  loop executor explicitly. Added a multi-worker regression test that fans
  8 × 2 parallel `FileRead` batches through an `AgentPromptRunner` on a
  4-io-worker runtime.

### Design Intent

`ToolScheduler`'s per-path lock table is single-strand by contract
(`src/oran-agent/_impl/path_lock_table.hpp`), and `run_batch`'s phase-2
cancellation emits must be serialized with its spawned children because
`asio::cancellation_signal` is not thread-safe. Slice 255 honoured that
contract for `--serve` (shared scheduler + loops on the service strand) but
the CLI and desktop paths kept spawning loop + scheduler on the multi-worker
executor — a latent data race whenever `runtime.workers > 1` and a provider
turn emits ≥ 2 `tool_use` blocks. The tracker offered two shapes: per-call-site
strands (the `--serve` precedent) or an internal scheduler strand. The
internal-strand shape was rejected because `run_batch`'s caller coroutine
cannot hop executors mid-flight, so the batch driver would need a
cross-executor spawn bridge, and a private strand would break the slice-255
reap-on-service-strand pattern (two strands over one pool do not serialize
with each other). Per-run strands keep one serialization domain per agent
loop while blocking work still hops to `cpu_executor()`, so bounded-parallel
tool batches keep overlapping on real I/O. The desktop strand additionally
serializes `ChatBridge::request_stop`'s posted emit with `begin_turn`'s
signal replacement.

### Files Modified

- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_prompt_runner.cpp`
- `docs/histories/2026-07/20260705-0132-cli-desktop-agent-strand.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/exec-plans/tech-debt-tracker.md` — removes the closed 2026-06-20
  scheduler-executor row (fix + regression test both landed).
- `docs/design-docs/tool-runtime.md` — Scheduler Boundary status note: every
  production entry now honours the agent-strand contract.
- `docs/ROADMAP.md` — Tool scheduler + Agent loop rows: frontier advanced,
  next step trimmed to the remaining per-agent channel-strand granularity.
- `docs/STATUS.md` — bumps to slice 270 and refreshes `test-bootstrap` counts.
- `docs/QUALITY_SCORE.md` — refreshes `oran-bootstrap` test counts.
- `docs/releases/feature-release-notes.md` — user-visible reliability note.

### Validation

- Commands run: `xmake build orangutan`, `xmake build test-bootstrap`,
  `xmake run test-bootstrap` (187 cases / 1832 assertions), full
  `xmake test` 19/19, `-fsyntax-only -DORAN_ENABLE_DESKTOP` compile of
  `bootstrap.cpp` for the gated desktop block, `make ci`.
- Tests added/changed: `AgentPromptRunner multi-tool batches complete on a
  multi-worker runtime` (`[prompt_runner][scheduler][concurrency]`) — the
  tracker's requested ≥2-tool batch on `workers > 1` regression, also the
  thread-safety probe under `--sanitizers=y`.
- Bench impact: none — a strand over the io pool adds no work to the tool
  handlers' blocking path.
- Compile-budget delta: none measured; no new includes beyond
  `<asio/strand.hpp>`-adjacent headers already reachable from
  `oran/async/runtime.hpp`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none added; one closed (2026-06-20 scheduler executor).
- Linked release note: `docs/releases/feature-release-notes.md`.

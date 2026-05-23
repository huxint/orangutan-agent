# 0017 — Fake-Provider-First Agent Loop

## User Problem

Spec 0001 lists the user-facing MVP for the ReAct loop ("paste prompt,
agent reasons + calls tools + answers"). It does **not** say *in what
order* the loop's pieces ship. The natural temptation is to bring up the
Anthropic adapter first, then write the loop against it. The deep-review
+ agent-loop-foundation notes both argue against that order:

- The loop's internal contract becomes whatever the Anthropic SDK
  happens to do. Provider-specific quirks (streaming event shape,
  stop-reason vocabulary, tool-result block structure) leak into the
  loop and every later adapter inherits them.
- Loop behaviour (cancellation, retry, tool ordering, transcript
  assembly) becomes hard to test because network, keys, rate limits,
  and provider non-determinism all enter the test loop.
- Bugs in the loop and bugs in the adapter become indistinguishable.
- Pinning regressions requires a real API account — small teams
  cannot run CI on every PR.

The agent-loop-foundation note's argument is the load-bearing claim:
*The first real `oran-agent` loop should run against a fake provider
before any Anthropic/OpenAI adapter is wired in.* This spec turns that
claim into a build-order contract — the fake provider, the internal
`provider::Request` / `provider::Response` shapes, and the ten
fake-scenario test matrix all ship before a single byte hits a real
vendor.

This is a **sequencing + harness spec**, not a replacement for spec
0001. Spec 0001 still owns the end-to-end user-visible MVP; this spec
owns *how the loop arrives there safely*.

## Scope (v1)

The MVP is the fake-provider-first sequence plus the test harness that
proves the loop behaves correctly without a network.

- **Internal provider contract**. The shapes already sketched in
  [`../design-docs/api-portability.md`](../design-docs/api-portability.md)
  "Domain Model" are the source of truth (`provider::Request`,
  `provider::Response`, `core::Content` variant, `StopReason`, `Usage`,
  `EventSink`). This spec freezes their *behaviour* for v1:
  - **Status (slice 75, 2026-05-24):** `oran-provider` now ships the
    value-type `Request`, `Response`, `Usage`, and `RetryPolicy` shapes
    plus prompt-cache hints, the abstract `provider::System` with
    `send(Request, Route, EventSink*) const`, the `provider::EventSink`
    streaming observer with default no-op callbacks for text / thinking /
    tool-start / tool-input-delta plus the terminal `on_done(StopReason)`,
    the `ProtocolKind` / `ModelTarget` / `Route` value shapes the loop will
    resolve once per turn, and the first concrete `provider::FakeProvider`
    that walks a `std::vector<ScriptedTurn>`, opens/extends typed content
    blocks from `StreamDelta`s, fans the same deltas through the supplied
    sink, awaits scripted latency via `async::sleep_for` so parent
    cancellation interrupts the wait, serialises concurrent `send` calls,
    and returns `Error::internal` on plan exhaustion or empty-body turns.
    Slice 75 adds the first `agent::Loop` driver over that contract:
    a one-iteration text-turn path that builds `prompt::RenderedPrompt`,
    maps prompt-cache hints, mirrors active/promoted tools into
    name-sorted `provider::Request::tools`, sends through a supplied
    `provider::System`, returns terminal text responses, forwards provider
    errors unchanged, and rejects `tool_use` responses with an explicit
    not-yet-implemented error. Real protocol adapters and multi-iteration
    tool dispatch remain downstream.
  - The loop emits **one** `provider::Request` per iteration.
  - The provider emits **one** `provider::Response` per request (after
    streaming completes if streaming is enabled).
  - `Response::blocks` is a typed `core::Content` vector: text,
    `tool_use`, `thinking`. The loop never reaches into vendor JSON.
  - `StopReason ∈ { end_turn, tool_use, max_tokens, stop_sequence,
    cancelled, error }`. Adapters map vendor stop reasons into this
    set; unknown vendor reasons map to `error`.
  - `Usage` is `{ input_tokens, output_tokens, cache_creation_tokens,
    cache_read_tokens, cost_estimate? }`. Missing fields are zero, not
    `nullopt`.
- **`provider::FakeProvider`** — first real implementation of the
  `provider::System` contract:
  ```cpp
  class FakeProvider : public provider::System {
   public:
    // A scripted plan; the loop drives the plan one Request at a time.
    explicit FakeProvider(std::vector<ScriptedTurn> plan);

    Awaitable<core::Result<Response>>
    send(Request, Route, EventSink* sink = nullptr) const override;
  };

  struct ScriptedTurn {
    // Either a complete assistant Response...
    std::optional<Response> response;
    // ...or a chain of streaming deltas that assemble into one.
    std::vector<StreamDelta> deltas;
    // ...or a failure to inject.
    std::optional<core::Error> error;
    // Optional latency for cancellation / timeout testing.
    std::chrono::milliseconds latency{0};
  };
  ```
  - Lives in `oran-provider` (or a `oran-provider-fake` neighbour
    library to keep the public include surface clean — implementation
    chooses; the load-bearing claim is *the fake is part of the
    library inventory*, not buried in `tests/`).
  - The fake provider is the only "provider" in CI for v1. The
    Anthropic adapter ships behind a config-gated profile in v1.1.
- **Ten fake-provider scenarios** (the matrix the agent-loop-foundation
  notes list explicitly):
  1. Assistant returns final text directly.
  2. Assistant requests one tool, gets a result, returns final text.
  3. Assistant requests multiple tools in one response.
  4. Assistant requests a missing tool (not in catalog).
  5. Tool succeeds, then assistant returns final text.
  6. Tool fails (handler returns error), assistant repairs or stops.
  7. Provider returns retryable error
     (`ErrorCategory::network` / `rate_limit` / `upstream`).
  8. Provider returns fatal error
     (`ErrorCategory::auth` / `invalid_request`).
  9. Cancellation arrives while waiting for the provider.
  10. Cancellation arrives while waiting for tool dispatch.
  Each is a single test file under `tests/agent/scenarios/`, each
  uses the fake provider with a hand-written plan.
- **`agent::Loop` MVP**. Wraps the seven phases listed in the deep
  review §What a better `oran-agent` should look like:
  - **Status (slice 75, 2026-05-24):** `<oran/agent.hpp>` exports
    `agent::Loop`, `LoopOptions`, `RunTurnInputs`, and `RunTurnResult`.
    The first implementation covers phase 3/4/5 only for scenario #1:
    render prompt, send one provider request, and return terminal text
    blocks. It intentionally does not dispatch tool calls yet; seeing
    `StopReason::tool_use` or a `ToolUseContent` block returns an
    `Error::internal` so tests cannot treat the first slice as a complete
    ReAct loop.
  1. Build `TurnContext` (identity, route, session id, origin,
     cancellation slot, stable service refs).
  2. Load/render memory once per turn (memory: `nullopt` in v1 — the
     hook exists; the renderer ships in spec 0005's slice).
  3. Render the prompt via `prompt::Builder` (spec 0016; the slice-70
     builder skeleton already owns section order, active/deferred catalog
     rendering, prefix hashes, and breakpoint placement, and the slice-71
     `prompt::PromotionState` snapshot can feed promoted deferred tools into
     the next active catalog. Slice 72 adds `agent::SessionState` as the
     session owner that observes successful `tool.search` output and performs
     that promotion before the full loop lands).
  4. Send `provider::Request` with streaming sink.
  5. Parse response blocks into typed `core::Content`.
  6. For each `tool_use`: validate schema → dispatch through
     `tool::Registry` (later via `agent::ToolScheduler`, spec 0012) →
     run approval render via `publish_blocking` (later, spec 0015) →
     append `tool_result` to working memory + session log.
  7. Stop on terminal `StopReason` or iteration cap.
- **Iteration cap** — `config.agent.max_iterations` (default 16) so an
  infinite tool-call loop terminates with `StopReason::error` and
  `reason=iteration_cap`. The cap is a runtime invariant, not a
  vendor concern.
- **Per-turn audit envelope**. `agent::Loop::run_turn` emits exactly
  one `AuditEvent` per tool call (already true via
  `Registry::dispatch`) plus one *turn-level* audit row recording
  `(agent_id, session_id, turn_id, iteration_count, stop_reason,
  total_usage, wall_time)`. The turn row is the parent of every
  child tool audit row through `parent_event_id` — the field already
  exists in `permission::AuditEvent::context` but is unused.
- **CI runs against the fake provider only**. v1 CI gate:
  `xmake test test-agent` exercises all ten scenarios; no network
  is required, no API key is required, no flake budget is needed.

## Scope (v1.1)

- **Anthropic Messages adapter** as the first real `protocol::Adapter`.
  Ships *after* v1 with no loop changes — the adapter satisfies the
  `provider::System` interface defined in v1.
- **OpenAI Responses adapter** in the same slice (or the next),
  parity gated by a future `bench-provider` protocol-overhead scenario.
- **Streaming sink** — `provider::EventSink` becomes a real coroutine
  channel surfaced through `oran-cli` so the REPL renders deltas
  character-by-character (spec 0001 acceptance #3).
- **Provider retry / fallback policy** — `execution::Runtime` lands
  here; the fake provider already exercises retry & fatal-error paths
  so the new code path is testable without a real network.

## Scope (v2)

- **Replay harness** — a recording of a real Anthropic conversation
  can be replayed against the fake provider for regression testing.
  The recording format is `(Request, Response)` pairs in JSON; the
  fake provider's `ScriptedTurn::response` already accepts this
  shape.
- **Adversarial provider fixtures** — fakes that emit malformed JSON,
  truncated streams, and unknown stop reasons. v1 catches the common
  cases; v2 catches the malicious ones once the loop is mature.
- **Multi-agent scheduling** — the loop becomes one of several agents
  on a process; spec 0004 (agent team collaboration) owns the
  scheduler.

## Out Of Scope

- A general-purpose mock framework. The fake provider is hand-written
  for clarity; tests author plans in C++ directly. No DSL.
- A vendor-equivalence test suite. The fake provider does not promise
  to emit Anthropic-equivalent bytes; it promises to emit *valid
  internal* `Response` shapes that exercise the loop.
- Network failure injection inside the real adapter. Once Anthropic
  is wired, the fake covers loop behaviour; network-layer chaos
  testing is `oran-http`'s job (existing test bucket).

## Acceptance Criteria

1. **Single-text turn.** Scenario #1: the loop sends one `Request`,
   the fake returns one text-only `Response` with
   `StopReason::end_turn`, the loop returns the assembled text to the
   caller, emits exactly one turn audit row and zero tool audit rows.
2. **Single-tool turn.** Scenario #2: the loop sends a `Request`, the
   fake returns one `tool_use` block, the loop dispatches the tool
   (using the existing `file.read` built-in), appends the tool result,
   sends a second `Request`, the fake returns final text. Audit
   records: one turn row, one tool row; the tool row's
   `parent_event_id` matches the turn row's id.
3. **Multiple tools in one response.** Scenario #3: the fake returns
   `[tool_use A, tool_use B]`. v1 dispatches sequentially (parallel
   dispatch is spec 0012's responsibility); the agent transcript
   contains both `tool_result`s in original `tool_use` order
   regardless of execution order; the next `Request` carries both
   results.
4. **Missing tool.** Scenario #4: the fake returns
   `tool_use { name: "tool.does_not_exist" }`. The loop synthesises a
   `tool_result` carrying an error message ("tool not found"); the
   audit row records `outcome=error,
   error_kind=tool_not_found`; the next `Request` carries the error
   text so the model can repair. The loop does not crash.
5. **Tool failure repair.** Scenario #6: the tool dispatch returns
   `Error::io`; the loop synthesises a `tool_result` with the error
   message; the next `Request` carries it; the fake returns either a
   repaired tool call or final text. Both paths terminate normally.
6. **Retryable provider error.** Scenario #7: the fake returns
   `core::Error{ category: network }` once, then a successful
   `Response`. The loop's retry logic
   (`api-portability.md` "Execution Layer") sends the same `Request`
   again; the second send succeeds; the audit row records
   `retries=1`. (Until spec 0001 v1.1 wires retry, this scenario
   asserts the loop *forwards* the retryable error unchanged for the
   next slice to handle — current criterion: the error reaches the
   caller with `Error::retryable() == true`.)
7. **Fatal provider error.** Scenario #8: the fake returns
   `core::Error{ category: auth }`. The loop does *not* retry;
   returns the error to the caller; emits one turn row with
   `stop_reason=error`.
8. **Cancellation during provider await.** Scenario #9: the fake
   sleeps `latency=10s`; the parent token fires at `t=100ms`. The
   loop returns `Error::cancelled` within `< 200ms`; the audit row
   records `stop_reason=cancelled, reason=parent_cancelled`.
9. **Cancellation during tool dispatch.** Scenario #10: the fake
   returns one `tool_use`; the tool handler sleeps long; the parent
   token fires; the loop returns `Error::cancelled` within `< 200ms`;
   the tool audit row records `outcome=cancelled`.
10. **Iteration cap.** A fake that returns
    `tool_use` blocks forever causes the loop to terminate with
    `StopReason::error, reason=iteration_cap` after
    `config.agent.max_iterations` iterations. Audit row records the
    cap; no infinite loop.
11. **CI runs offline.** `xmake test test-agent` passes with no
    network access and no API key in env. Pinned by a CI job that
    explicitly unsets `ANTHROPIC_API_KEY` / `OPENAI_API_KEY` /
    `https_proxy` before running.
12. **Anthropic-adapter shipping without loop change.** When v1.1
    ships the Anthropic adapter, the v1 acceptance criteria above
    still pass *without modification* to `agent::Loop`. Pinned by
    re-running the fake-provider test bucket against the loop after
    the adapter lands.

## Design Doc Cross-References

- [`../design-docs/api-portability.md`](../design-docs/api-portability.md)
  — `provider::Request`, `provider::Response`, `core::Content`,
  `StopReason`, `Usage`, `EventSink`. This spec consumes the design
  doc's shapes; the design doc owns *what they look like*, this spec
  owns *what behaviour they pin in v1*.
- [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)
  — six cross-cutting concerns; this spec's loop is the first
  enforcement point for cancellation, identity, and observability.
- [`0001-core-react-loop.md`](0001-core-react-loop.md) — owns the
  end-user MVP; this spec is the *sequencing* sub-spec underneath.
  Spec 0001's "Execution Plan" placeholder is replaced by a
  `docs/exec-plans/active/<date>-agent-mvp.md` whose first slice is
  fake-provider only.
- [`0012-tool-scheduler-and-state.md`](0012-tool-scheduler-and-state.md)
  — `agent::ToolScheduler` becomes the v1.1 replacement for the
  sequential dispatch in scenario #3.
- [`0015-blocking-hook-decisions.md`](0015-blocking-hook-decisions.md)
  — `publish_blocking` consumption lands in v1.1 once the operator
  prompt sink exists; v1's loop only emits advisory publishes.
- [`0016-prompt-and-tool-catalog-cache.md`](0016-prompt-and-tool-catalog-cache.md)
  — `prompt::Builder` is the loop's prompt source from v1 onward.
- [`0014-structured-tool-output.md`](0014-structured-tool-output.md)
  — `tool_result` blocks carry `Output::data` when present; v1 falls
  back to text per the migration plan.
- [`0018-first-loop-observability.md`](0018-first-loop-observability.md)
  — defines the trace shape; this spec's loop emits the rows.

## Risks

- **Fake-provider drift from reality.** A fake that lets you exercise
  responses real vendors never emit. Mitigation: the v2 replay
  harness records real conversations and replays them; until then
  the fake is the *contract*, not the *vendor* — adapter bugs are
  caught in a future `bench-provider` protocol-overhead scenario.
- **Stop reason mapping mistakes.** Adapters that map a vendor stop
  reason to the wrong internal `StopReason` look fine in the fake
  but loop forever in production. Mitigation: every adapter test
  asserts the stop-reason mapping table; the table lives in the
  adapter TU, not in the loop.
- **Tool dispatch coupling.** v1 dispatches sequentially in the
  loop; v1.1 delegates to `ToolScheduler`. The migration must
  preserve audit + hook semantics exactly. Mitigation: the spec
  0012 acceptance criteria already pin per-call audit + hook
  invariants; the migration is a one-call substitution at the loop
  site.
- **Iteration cap surprises.** A cap of 16 may be too low for
  research-style workflows. Mitigation: per-route override
  (`route.max_iterations`) ships when the first real workflow needs
  it; v1 ships a single global cap.

## Validation

```sh
xmake build oran-agent oran-provider
xmake test test-provider                    # provider domain/cache mapping
xmake run test-agent                        # current text-turn Loop + session-state coverage
xmake build bench-agent
xmake run bench-agent                       # prompt-cache fixture; loop-overhead bench still future
unset ANTHROPIC_API_KEY OPENAI_API_KEY      # CI proves offline
xmake run test-agent
```

## Out-of-Band Cross-Cuts

- `docs/ARCHITECTURE.md` — `oran-agent` flips from "planned" to
  "skeleton" in the slice that lands v1; `oran-provider` already carries the
  request/response/cache-hint shapes and later ships `FakeProvider`.
- `docs/design-docs/agent-platform.md` "Goals For The First 12
  Months" — the MVP runtime goal #1 gets a sub-point pointing at
  this spec for the v1 sequencing.
- `docs/exec-plans/active/<date>-agent-mvp.md` — created when the
  first slice of v1 starts; the plan's first slice is exactly the
  v1 acceptance criteria above.
- `docs/design-docs/api-portability.md` "See Also" — gains a row
  pointing at this spec as the consumer of the domain model.
- `docs/STATUS.md` — `oran-agent` reaches `C` (per
  [`QUALITY_SCORE.md`](../QUALITY_SCORE.md)) when v1 ships.

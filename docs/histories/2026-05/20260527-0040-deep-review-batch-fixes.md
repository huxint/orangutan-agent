## [2026-05-27 00:40] | Task: 2026-05-26 deep-review batch absorption

### Execution Context

- Agent: Claude Opus 4.7
- Base model: claude-opus-4-7
- Runtime: Claude Code (multi-agent deep-review workflow + main-loop
  implementation)
- Linked plan: none — single-slice bundle of remaining 2026-05-26
  deep-review findings per user request (`后续的可以一次性全部修复`).

### User Query

> 对项目进行 deep-review 找出实现中的不足点和缺陷/bug/性能 issue, 然后fix.
> 提交信息格式标准, 详细. 后续的可以一次性全部修复, 不用分为更细的提交.
> (Do a deep review of the project, find bugs/perf issues, then fix. Detailed
> commit messages. Bundle the remaining fixes in one commit.)

### Changes Overview

- Areas: `oran-agent` (loop trace writers + cancelled stop terminal),
  `oran-provider` (Response field + execution context + OpenAI encoder +
  decoder), `oran-bootstrap` (parse_args strictness + prompt runner factory +
  signal-drain header), `oran-io` (singleflight leader cancel guard +
  cross-executor wake), `oran-core` (BoundedCache eviction reason),
  `oran-permission` (erase_if), `oran-storage` (drop duplicate helper),
  `oran-config` (static_cast<void>).
- Key actions follow the deep-review finding IDs:
  - **F1 + F18 (high/low bug)** — `agent::Loop::run_turn` now preserves the
    original provider/tool/loop-boundary error when the matching
    `write_*_trace_turn` call fails. Each of the ten error-path call sites
    attaches `trace_write_failed=<message>` via a new
    `attach_trace_write_error` helper and still returns the cause; the
    terminal-success and iteration-cap paths keep the existing behavior
    (those paths have no prior cause to preserve).
  - **F5 (medium bug)** — `provider::Response` gains an optional
    `route_profile_used`, `provider::execution::Runtime` fills it from the
    served target's profile (mirroring how `model_used` is filled), and the
    loop's `make_trace_request` now consumes it so `trace_turns.route_profile`
    describes the profile that actually answered the request when a
    `Route::fallbacks` entry won.
  - **F6 + F12 (medium + low bug)** — `bootstrap::run` parse_args (a) tightens
    the `--audit-init` and `--trace` optional-path sniff to reject any token
    starting with `-` (so `orangutan --audit-init -h` no longer creates a
    directory named `-h`), and (b) rejects duplicate `--config`,
    `--audit-init`, and `--trace` occurrences with the same
    `arg_error("...may be provided only once")` shape `--prompt` has used.
  - **F9 (medium bug)** — `agent::Loop` extends the terminal-arm match to
    include `core::StopReason::cancelled`, so an OpenAI Responses
    `status="cancelled"` reaches the success path (the trace row records
    `stop_reason=cancelled` rather than the misleading `non-terminal stop
    reason` error trace row + `Error::internal`).
  - **F10 (medium bug)** — `provider::execution::Runtime::send` wraps the
    retry-backoff sleep error in `with_target_context(target, attempt,
    max_attempts)` so cancellation-during-backoff and any other timer
    failure carry the same `provider_profile / provider_model / attempt /
    max_attempts` context every other early-exit attaches.
  - **F11 (medium bug)** — `provider::make_openai_responses_request` now
    filters `core::Role::system` messages from the iteration loop and folds
    their text into `body.instructions` via a new
    `append_openai_instructions_text` helper (mirroring the existing
    `append_anthropic_system_text`); non-text blocks under a system role
    reject as `protocol_error`. A system message in `request.messages` no
    longer double-emits as both `body.instructions` and
    `input[].role==system`.
  - **F3 + F4 + F23 (high leak + high race + low dead)** — the `oran-io`
    `read_text_file_ranged` singleflight (a) wraps the leader's completion
    in an RAII `LeaderCompletionGuard` whose destructor publishes a
    `singleflight_leader_unwound` cancelled result on any unwind, so a
    leader cancelled mid-`read_text_file_cold_async` can no longer orphan
    every follower's timer; (b) routes the wake-up to each waiter's own
    executor via `asio::post(waiter->get_executor(), …)` so the leader
    never mutates a `basic_waitable_timer` from a foreign thread (asio
    documents `basic_waitable_timer` as "shared objects: Unsafe"); and
    (c) drops the redundant pre-cold `co_await asio::post` on the leader.
  - **F14 (low style)** — `static_cast<void>(value)` replaces the C-style
    `(void)value;` discard in `src/oran-config/config.cpp` per the C17
    "C++ Over C Idioms" rule.
  - **F15 / F13 (low style + low docs)** — `signal_name` returns
    `std::string_view` instead of `const char*`, and the signal_drain
    docstring stops referencing a nonexistent `make_signal_cancelled_error`
    helper (it now describes the real source — `bootstrap::run`'s
    SIGINT/SIGTERM trap).
  - **F16 + F20 (low style)** — `AgentPromptRunner` factory uses
    `std::make_unique<AgentPromptRunner>(impl, PrivateTag{})` with a
    passkey-tagged public constructor instead of `std::unique_ptr<T>{new
    T{...}}`. The passkey's default constructor is private and only
    `AgentPromptRunner` is a friend, so external callers still cannot
    construct one.
  - **F22 (low bug)** — `core::BoundedCache::put` introduces
    `EvictionReason::invalidated` for the same-key-overwrite and
    oversize-rejection paths so those mutations no longer double-count
    against `evictions_bytes` / `evictions_lru`. The new reason has no
    counter increment in `drop_locked`'s switch; the existing eviction
    counters now describe only budget-pressure evictions.
  - **F24 (low style)** — `permission::ApprovalBroker::reap_expired` uses
    `std::erase_if(grants_, pred)` instead of a hand-rolled iterator erase
    loop.
  - **F25 (low style)** — `storage::trace_repository` drops the local
    `is_zero_id` helper and calls the shared `core::is_zero_turn_id`
    directly at every call site.

### Design Intent

The 2026-05-26 multi-agent deep review surfaced 25 verified findings across
nine library areas. Slice 113 absorbed the two AgentPromptRunner-specific
findings (F2 and F19) as its own slice. The user asked for the remaining
fixes to land in one bundle so the per-area history would be coherent (a
finding-by-finding split would have produced eight tiny slices and obscured
the shared "deep-review-2026-05-26 absorption" thread).

Each fix is local and well-scoped. The three most impactful are:

1. **Trace-write error preservation (F1 + F18).** The 10 error-path call
   sites all had the same shape: write a trace row, return the trace
   storage error if the write failed, otherwise return the original error.
   Returning the storage error in place of the cause makes operators chase
   an `invalid_argument` from `validate_append_request` (e.g. an empty
   `agent_key`) when the real problem was a `network` provider error. The
   fix preserves the original error and attaches the trace failure as
   non-critical context — the operator still learns the trace write
   failed, but they learn it second, not first.

2. **`route_profile` under fallback (F5).** The provider execution wrapper
   already filled `model_used` with the served target's model. Without
   the matching `route_profile_used`, `trace_turns.route_profile` claimed
   the primary profile served the response even when the wrapper fell
   back. A future operator inspecting traces would not know which profile
   actually responded. The fix mirrors `model_used` exactly: optional
   field on `Response`, filled by the execution wrapper when the backend
   does not set it, threaded into the loop's trace writers.

3. **oran-io singleflight (F3 + F4 + F23).** The cross-executor wake was a
   latent UB risk that only fires if a follower and leader run on
   different threads (which is the common case under
   `Runtime::cpu_executor()`). The cancel-leak was a starvation bug: a
   cancelled leader's followers would wait forever. The RAII guard publishes
   a cancelled result on any unwind path, and routing wakes through each
   waiter's executor keeps every mutation of a `basic_waitable_timer` on
   its owning strand.

### Files Modified

- `include/oran/provider/types.hpp` — added `Response::route_profile_used`.
- `src/oran-provider/execution.cpp` — fills `route_profile_used` on
  primary/fallback success; enriches retry-backoff sleep errors.
- `src/oran-provider/protocol_request.cpp` — added
  `append_openai_instructions_text` and folds `Role::system` messages.
- `src/oran-agent/loop.cpp` — added `attach_trace_write_error`,
  `last_route_profile`, threads `served_profile` through every
  `write_*_trace_turn` call, preserves the original error on trace write
  failure, and extends the terminal arm to recognize
  `StopReason::cancelled`.
- `src/oran-bootstrap/bootstrap.cpp` — strictness rewrites for
  `--audit-init / --trace / --config` (short-flag sniff + duplicate
  rejection); bumped binary slice tag.
- `include/oran/bootstrap/prompt_runner.hpp`,
  `src/oran-bootstrap/prompt_runner.cpp` — passkey-protected public
  constructor + `std::make_unique` factory.
- `include/oran/bootstrap/signal_drain.hpp`,
  `src/oran-bootstrap/signal_drain.cpp` — `string_view` return + corrected
  docstring.
- `src/oran-io/file.cpp` — singleflight RAII guard, cross-executor wake,
  redundant-post removal.
- `include/oran/core/bounded_cache.hpp` — new `EvictionReason::invalidated`.
- `src/oran-permission/approval_broker.cpp` — `std::erase_if` rewrite.
- `src/oran-storage/trace_repository.cpp` — drop local `is_zero_id`.
- `src/oran-config/config.cpp` — `static_cast<void>` discard.
- Tests: `tests/agent/test_loop.cpp` (+ trace-write error preservation),
  `tests/provider/test_execution.cpp` (+ `route_profile_used` filled,
  + retry-backoff target context), `tests/provider/test_protocol_request.cpp`
  (+ OpenAI system message dedup), `tests/provider/test_protocol_response.cpp`
  (+ OpenAI cancelled status), `tests/bootstrap/test_bootstrap.cpp`
  (+ `--audit-init -h` rejection, + duplicate-flag rejection).

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 114, pointed at this history entry,
  refreshed `test-agent` / `test-bootstrap` / `test-provider` test counts,
  rewrote the next-intended-slice paragraph, and contracted the
  `review/deep-2026-05-26` tracker row to the remaining bullets.
- `docs/exec-plans/tech-debt-tracker.md` — same row contraction; the only
  remaining items are a singleflight regression test scaffold and a
  forward-looking caveat about future cache stat consumers.
- `docs/product-specs/0018-first-loop-observability.md` — F8: replaced the
  three "ordinary binary handoff remains downstream" wordings with the
  slice-112 reality (the runner now drives configured-route
  `bootstrap::run` via `HttpProviderBackend`).

### Validation

- Commands run:
  - `xmake build` over every affected library (`oran-core`, `oran-async`,
    `oran-io`, `oran-storage`, `oran-config`, `oran-permission`,
    `oran-hook`, `oran-tool`, `oran-prompt`, `oran-provider`, `oran-agent`,
    `oran-bootstrap`, `oran-cli`).
  - All 14 per-library test runners: `xmake run test-core test-async
    test-io test-http test-storage test-config test-permission test-hook
    test-tool test-prompt test-cli test-provider test-agent test-bootstrap`.
  - `scripts/check-status-fresh.sh` (passed; latest history pointer
    matches the newest file under `docs/histories/`).
- Test counts changed:
  - `test-agent`: 25 cases / 401 assertions → **26 / 407** (+1 case, +6
    assertions for trace-write error preservation).
  - `test-bootstrap`: 70 / 308 → **72 / 316** (+2 cases, +8 assertions for
    `--audit-init -h` rejection and duplicate-flag rejection).
  - `test-provider`: 63 / 512 → **66 / 528** (+3 cases, +16 assertions for
    `route_profile_used`, OpenAI cancelled status, OpenAI system-message
    dedup).
  - The other 11 suites are unchanged at 71/455, 9/43, 49/286, 3/21,
    72/899, 33/241, 89/426, 30/207, 178/1838, 10/98, 14/97.
- Bench impact:
  - No new bench. The trace writers, parse_args, and provider mappers all
    run once per turn / per startup / per protocol round trip; the
    singleflight wake gains an `asio::post` per follower which is
    dominated by the existing `async_wait`/timer cost.
- Compile-budget delta:
  - Not measured. The new `attach_trace_write_error`,
    `append_openai_instructions_text`, and `LeaderCompletionGuard` are
    small private helpers; no new third-party includes; the only public
    surface change is the optional `Response::route_profile_used` field,
    which is a trivial `std::optional<std::string>`.

### Follow-ups

- Tech-debt entries: `review/deep-2026-05-26` is contracted but still
  open — a dedicated singleflight regression test scaffold (deterministic
  cross-executor interleavings) remains a follow-up. The `/tmp/`
  provenance artifact stays at `/tmp/orangutan-deep-review-2026-05-26.md`
  and is delete-on-close per `rules/deep-review.md` when the singleflight
  test lands.
- Issues opened: none.
- Linked release note: none — these are correctness fixes for behavior
  that was never documented as shipped, plus follow-on style cleanup.

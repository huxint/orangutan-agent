## [2026-05-17 23:59] | Task: wire `permission::ApprovalBroker` through `Registry::dispatch` so `Verdict::ask` is mediated instead of short-circuited

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — single-session slice that fits the
  `Next intended slice` bullet in
  [`STATUS.md`](../../STATUS.md) ("the approval-broker flow that
  replaces the `Verdict::ask` short-circuit in `Registry::dispatch`,
  now uniform across all four file built-ins"), matching
  `PLANS_GUIDE.md` "When NOT To Create A Plan".

### User Query

> 深度了解项目，查看当前项目真实进度, 继续推进项目代码的实现.
> ultrathink. 直到一整个大模块的完成.
>
> (Understand the project deeply, check the real current progress,
> continue advancing the project code implementation. Ultrathink.
> Until a complete major module is done.)

The user re-issued the autonomous-progress prompt and explicitly asked
for "a complete major module" of work. Of the three remaining
`Next intended slice` candidates (approval-broker wiring, first Anthropic
adapter, signal-aware shutdown), the approval-broker wiring is the
candidate that *closes* an existing module — the permission system,
which already shipped audit / broker / authority / materialize / config
plumbing across slices 7–16. The dispatch wiring is the last piece that
makes that infrastructure observable from end-user tool calls; the
Anthropic adapter is genuinely multi-slice (transport + protocol +
execution + integration tests) and needs an exec plan, while the
signal-aware shutdown is low-value mid-construction. Slice 21 picks the
approval-broker wiring and rounds out the permission module so the
agent-loop slice can inherit a fully working ask flow.

### Changes Overview

- **`DispatchContext` extension.** Three new fields:
  - `permission::ApprovalBroker* approval_broker{nullptr}` — non-owning
    pointer the agent loop sets when it has a long-lived broker in
    hand (today: `bootstrap::RuntimeAssembly::approval_broker()`).
  - `const permission::ApprovalToken* approval_token{nullptr}` — non-
    owning pointer the agent loop sets after `broker.approve()` returns
    a token on the second turn of an ask flow.
  - `core::Time now{}` — wall-clock instant the broker uses to evaluate
    `expires_at`. Default-init is `Time::epoch()` (UNIX epoch); the
    agent loop sets it from `core::time::now_utc()` per call.
  - The header is now explicit about its dependency on
    `<oran/permission/approval.hpp>`, `<oran/permission/approval_broker.hpp>`,
    and `<oran/core/time.hpp>`. Forward-declarations are not enough
    because the optional fields use full types (pointer to incomplete
    type would deny the user the brace-init field names from inside
    `make_ctx`).
- **`Registry::dispatch` branching.**
  1. Lookup + rule evaluation are unchanged; the failure case still
     returns `Error::not_found`.
  2. Audit-event construction is unchanged (still derives `verdict` +
     `outcome` from `make_audit_event_from_decision(decision)` and
     stamps the identity columns + SHA-256 input hash).
  3. **New step.** When `decision.verdict == Verdict::ask` AND both
     `ctx.approval_broker` and `ctx.approval_token` are non-null,
     `dispatch` calls
     `ctx.approval_broker->check(*token, name, input_json, ctx.identity, ctx.now)`
     *before* recording the audit row, then mutates the in-flight event
     so the row carries the post-broker outcome:
     - on success: `outcome = AuditOutcome::approved` (reason is left as
       the rule reason — only rejections overwrite the reason to keep
       forensic queries simple).
     - on failure: `outcome = AuditOutcome::rejected` and the event's
       `reason` swaps from the rule reason to the broker's `reason`
       context entry (`expired` / `tool_mismatch` /
       `identity_mismatch` / `input_mismatch` / `mac_mismatch` /
       `no_grant` / `replay_exhausted`).
  4. Audit record happens once, with the (possibly remapped) outcome.
  5. **New step.** The `ask` branch of the post-record `switch`:
     - if the broker rejected, forward the broker's `core::Error`
       verbatim (only adding `tool=<name>` to its context).
     - if the broker accepted, fall through to the handler — the audit
       row already records `outcome=approved`.
     - if no broker or no token was supplied, take the legacy short-
       circuit path: return `permission_denied` with `reason=approval_required`
       *plus* three new context entries copied from `Decision`:
       `decision_reason`, `replay_max`, `approval_ttl_seconds`.
- **Error-context propagation.** The `approval_required` error now
  carries everything the agent loop needs to call
  `ApprovalBroker::approve` without re-running `RuleSet::evaluate`. The
  fields are stringified with `std::to_string` so they pass through
  the existing key/value context channel unchanged; the agent loop
  parses them back to integers when constructing the `ApprovalGrant`.
- **Audit-row semantics on rejection.** Rejection swaps `event.reason`
  from the rule reason (e.g. `"rule #0 (ask: noop)"`) to the broker
  reason (e.g. `"replay_exhausted"`) so a forensic query can directly
  tell *why* a call was rejected without re-running the broker. The
  rule reason is still recoverable from the rule set + the recorded
  `verdict` if a future audit consumer needs it.
- **Allow/deny verdicts skip the broker.** Even when both
  `approval_broker` and `approval_token` are present, an `allow`
  verdict runs the handler immediately and a `deny` verdict returns
  `permission_denied` with the rule reason — the broker is only
  consulted when the decision was `ask`. This matches the design-doc
  intent: a token is an answer to a *question* the rule set asked, not
  a universal "trust this call" stamp.
- **Tests.** `tests/tool/test_registry.cpp` grows to 50 cases / 414
  assertions (+10 cases, +105 assertions) covering:
  - The existing ask short-circuit case now asserts the three new
    error-context entries (`replay_max=8`, `approval_ttl_seconds=3600`,
    `decision_reason="rule #0 (ask: noop)"`).
  - A new case pins propagation of *custom* `replay_max=2` /
    `approval_ttl=120s` from the matched rule into the same context.
  - Approved happy path: `approve` → re-dispatch → `outcome=approved`
    + reason preserved as the rule reason.
  - Four rejection paths: `replay_exhausted` (`replay_max=0` grant),
    `no_grant` (broker reaped before the second dispatch), `expired`
    (dispatch `now` past the 60s TTL), and `tool_mismatch` (token
    issued for `alpha`, presented during `beta` dispatch).
  - Broker present but no token: short-circuit path is preserved AND
    `broker.outstanding_grants() == 0` is asserted (the broker should
    not be consulted on this path).
  - Allow/deny verdicts with a presented (invalid) token: the handler
    runs / the deny error fires *without* the broker being consulted.
  - End-to-end ask → approve → re-dispatch → exhaust: one ask row,
    three approved rows, one rejected row — total 5 audit events,
    asserted in order.
- **Bench.** New `bench/tool/scenarios/approval.cpp` registers three
  scenarios via `register_tool_approval`:
  - `dispatch_ask_short_circuit` ~2,385 ns — the baseline. Same shape
    as `registry.dispatch_allow` plus the new error-context build.
  - `dispatch_ask_approved` ~13,408 ns — broker + valid token
    (`replay_max=UINT32_MAX` so the bench loop never trips
    exhaustion). Adds the HMAC verify + map find + counter decrement
    on top of the baseline, then runs the trivial handler.
  - `dispatch_ask_rejected` ~13,570 ns — broker + exhausted token
    (`replay_max=0`). Same broker work as the approved path, but the
    handler is skipped.
  - (approved − short_circuit) ≈ 11 µs is the per-call broker-attached
    overhead the agent loop pays once it has approval in hand. Of
    that ~11 µs, ~10.7 µs is the raw `broker.check_ok` cost already
    pinned by `bench-permission/approval_broker`, so the registry-
    side overhead on top of the broker work is ~875 ns (the audit
    row mutation, the post-record switch, and the JSON `input_hash`
    re-computation inside the broker's `check`).
- **No xmake plumbing change.** `oran_lib` already globs `**.cpp` under
  `src/oran-tool/`, and `oran_bench` globs `bench/tool/**.cpp`, so the
  new `approval.cpp` is picked up without a target edit. `bench-tool`'s
  `main.cpp` adds a single `register_tool_approval` registration call.
- **Slice-version bump.** `kVersion` 20 → 21. `xmake run orangutan --help`
  reports `orangutan v2.0.0-slice21`.

### Design Intent

**Why the approval-broker wiring is the right closing slice for the
permission module.** Every other ingredient — `ApprovalSecret`,
`ApprovalAuthority`, `ApprovalToken`, `ApprovalBroker`, the
`AuditOutcome::approved` / `AuditOutcome::rejected` enumerators, the
`Decision::replay_max` / `Decision::approval_ttl` policy fields, the
`bootstrap::RuntimeAssembly::approval_broker()` getter — has been in
place for ~24h. The only thing missing was the *consumer*. Until this
slice, `Verdict::ask` always failed with `reason=approval_required`,
which meant the broker had no callers and the agent loop had no way to
exercise the approval flow. Wiring `Registry::dispatch` to consult the
broker turns the existing infrastructure into a feature an operator can
*see* in audit logs.

**Why the broker is a context field rather than a constructor argument
to `Registry`.** Two reasons. First, the broker's lifecycle is per-
process while the registry is per-strand — an agent that spawns sub-
agents wants those sub-agents to share *one* broker (and *one* audit
sink) but might want separate registries with different built-in
catalogs. Second, the token is per-call (the agent loop captures it
between turns), which means the registry can't own it. Putting both on
`DispatchContext` keeps the dispatch surface single-method and lets the
agent loop populate them per turn without re-constructing anything.

**Why pointers, not `std::optional<std::reference_wrapper<...>>`.** The
broker is naturally non-owning (the bootstrap assembly owns it; the
context borrows). The token is by value but cheap to address. Pointers
default-initialize to `nullptr` so the existing tests that brace-init
`DispatchContext` without these fields continue to compile and run
unchanged. The `optional<reference_wrapper>` shape would require
callers to opt out of brace-init for the broker field. Plain pointers
match the audit/rules/executor fields' "reference at runtime, may be
absent" semantics in the simplest way the language supports.

**Why `core::Time` is a value, not a callable.** A clock injected as
`std::function<core::Time()>` adds a function-call indirection per
dispatch and a heap allocation per `DispatchContext` rebuild — measurable
when dispatch is in the µs range. A value lets the agent loop compute
`core::time::now_utc()` once per turn and reuse it across the
`evaluate` + `broker.check` + audit-write pair, which is also the
correct semantics: the same call should be evaluated against the same
clock instant on both sides.

**Why the audit row's `reason` swaps to the broker reason on rejection
but stays as the rule reason on approval.** The forensic query "why was
this call rejected" wants a single, specific answer that the audit row
already has — `replay_exhausted` is what an operator needs to read on a
dashboard, not `rule #3 (ask: ...) | broker said replay_exhausted`. On
the approval path the broker did not contribute a reason (it just
returned `void`), so the rule reason is the correct thing to record.
The rule reason is still recoverable from the rule set + `verdict`
when the consumer wants to bridge the two.

**Why allow/deny verdicts ignore the broker.** A presented token is the
answer to a *previously asked* question. A new call whose verdict is
`allow` is not asking a question — the rule set already approved it. A
new call whose verdict is `deny` is being *forbidden* by an explicit
rule, which the design says should outrank any prior approval (matching
the "explicit deny always wins" precedence in `permissions-and-hooks.md`).
The slice could have made the broker veto allows too, but that would
have given the broker more authority than the rule set, which contradicts
the design.

**Why the new error context fields (`replay_max`, `approval_ttl_seconds`,
`decision_reason`) live on the error and not on a new accessor.** The
agent loop already inspects `Error::context()` for `reason=approval_required`;
adding sibling entries keeps the surface uniform and doesn't require a
new method on `Registry`. The fields stringify cleanly through the
existing `with(key, value)` channel — `std::to_string(decision.replay_max)`
and `std::to_string(decision.approval_ttl.count())`. The agent loop's
parser is one `std::stoul` per field on a known-numeric string, which
is cheap and impossible to mistype because the keys are documented in
the dispatch contract.

**Why bench's three-way A-vs-B-vs-C rather than a pair.** A pair would
have left "what does the broker cost?" answerable only by subtracting
two numbers from different runs, and `rejected` is informative in its
own right — the agent loop will encounter rejection on every stale
token, so pinning that cost separately means a future regression is
detectable. The bench shows the rejected path costs essentially the
same as approved (~13.6 vs ~13.4 µs), which confirms the broker pays
the MAC cost up front; the handler-skip on rejection is a wash against
the trivial in-process handler's nanoseconds-cost on approval.

### Files Modified

- `include/oran/tool/registry.hpp` — file header rewritten to cover
  the four-bullet composition (slice 21 broker bridge added); new
  includes for `core::Time` / `ApprovalToken` / `ApprovalBroker`;
  three new fields on `DispatchContext` with full docstrings; the
  `dispatch` contract docstring rewritten to list the five outcome
  branches and pin the new error-context entries.
- `src/oran-tool/registry.cpp` — new `broker_reason` helper; rewritten
  `dispatch` body with the verdict=ask broker branch; new
  `replay_max` / `approval_ttl_seconds` / `decision_reason` error-
  context entries on the short-circuit path; includes now pull
  `<optional>` + `<oran/permission/approval_broker.hpp>` explicitly.
- `tests/tool/test_registry.cpp` — existing ask short-circuit case
  extended with the three new context assertions; one new custom-
  policy propagation case; one new helper block (`make_broker`,
  `fixed_now`, `grant`, `make_approval_ctx`) that the new cases
  share; nine new broker-flow cases; the file now ends with the
  end-to-end ask → approve → re-dispatch → exhaust integration test.
- `bench/tool/scenarios/approval.cpp` — new TU registering the
  three-way A-vs-B-vs-C contrast described above.
- `bench/tool/main.cpp` — registers `register_tool_approval` after the
  existing four blocks.
- `bench/tool/README.md` — documents the new three-way scenario.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 20 → 21.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 21, history pointer, library surfaces row
  for `oran-tool` (50 cases / 414 assertions), and a refreshed
  `Next intended slice` bullet (down to the Anthropic adapter +
  signal-aware shutdown + hook bus scaffolding, since approval-broker
  is now done).
- `docs/QUALITY_SCORE.md` — Tool registry row rewritten to describe
  the new dispatch contract and the slice-21 dispatch numbers; Test
  framework row refreshed with `oran-tool` 50/414; Bench harness row
  extended with the new `dispatch_ask_short_circuit` /
  `dispatch_ask_approved` / `dispatch_ask_rejected` trio; Permissions
  row's "Next Step" cell flipped from "wire `permission::ApprovalBroker`
  into `Registry::dispatch`" to "render-side ask flow in `oran-agent`
  + hook bus".
- `docs/ARCHITECTURE.md` — slice-status preamble now lists slice 21
  alongside 17/18/19/20; `oran-tool` inventory row rewritten with the
  broker mediation language and the preserved short-circuit
  guarantees.
- `docs/design-docs/tool-runtime.md` — Status box updated to describe
  the new `DispatchContext` shape (approval_broker / approval_token /
  now) and the audit-outcome remapping.
- `docs/design-docs/permissions-and-hooks.md` — Engine-status box
  extended with the approval-broker dispatch-wiring paragraph,
  including bench numbers and a pointer to where the render-side ask
  flow (operator prompt + token capture) will land.
- `docs/releases/feature-release-notes.md` — new top row
  `oran-tool-approval-broker`.
- `docs/histories/2026-05/20260517-2359-oran-tool-approval-broker.md` —
  this file.

### Validation

- Commands run:
  - `xmake build oran-tool` — clean (6 TUs, ~12 s).
  - `xmake build test-tool` — clean (~30 s).
  - `xmake run test-tool` — 50 cases / 414 assertions, all green.
  - `xmake test` — all 9 buckets green
    (test-async / cli / core / config / io / tool / bootstrap /
    permission / storage).
  - `xmake build bench-tool && xmake run bench-tool` — clean;
    measured `registry.lookup ~7.66 ns`,
    `registry.dispatch_allow ~2,135 ns`,
    `file_write.dispatch_truncate ~12,419 ns`,
    `file_write.dispatch_append ~11,818 ns`,
    `file_edit.dispatch_unique_replace ~15,304 ns`,
    `file_edit.dispatch_replace_all_many ~16,865 ns`,
    `file_search.single_file_one_match ~7,915 ns`,
    `file_search.recursive_dir_many_matches ~26,209 ns`,
    `dispatch_ask_short_circuit ~2,385 ns`,
    `dispatch_ask_approved ~13,408 ns`,
    `dispatch_ask_rejected ~13,570 ns`.
  - `xmake build orangutan && xmake run orangutan -- --help` —
    prints the slice-21 banner; the CLI surface is unchanged.
- Tests added/changed: 10 new tool-bucket cases (+105 assertions); the
  existing ask short-circuit case was extended with three new
  context-entry assertions.
- Bench impact: existing scenarios unchanged within noise (the
  `registry.dispatch_allow` reading flagged as unstable by nanobench
  in this run sits at ~2.1 µs vs. the slice-20 reading of ~2.66 µs
  — within typical jitter for this scenario, no algorithmic change);
  new scenarios baselined above.
- Compile-budget delta: one new TU in `oran-tool` was *not* added —
  the work is inside the existing `registry.cpp`. One new TU in
  `bench-tool` (`approval.cpp`); its headers (`nlohmann/json` and re2
  stay out) overlap fully with the existing bench bucket's PCH-amortised
  set, so build-time impact is in the same envelope as the previous
  bench scenarios.

### Follow-ups

- Issues to file: none.
- Tech-debt entries: none filed for this slice. The two slice-20
  `file.search` tech-debt rows (regex support deferred,
  ripgrep-class optimisations deferred) are unchanged.
- Linked release note: 2026-05-17 `oran-tool-approval-broker` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: when the `oran-agent` ReAct loop
  lands, the natural integration is:
  1. The loop owns a `permission::ApprovalBroker&` borrowed from
     `bootstrap::RuntimeAssembly::approval_broker()` and a
     `permission::AuditSink&` borrowed from `audit_sink()`.
  2. Per turn, the loop builds *one* `DispatchContext` whose
     `approval_broker` is set, `approval_token` starts `nullptr`, and
     `now` is `core::time::now_utc()`.
  3. On `Verdict::ask` short-circuit (`reason=approval_required`), the
     loop reads `replay_max` + `approval_ttl_seconds` + `decision_reason`
     out of the error context, renders the prompt to the operator,
     and — if the operator approves — calls
     `broker.approve(ApprovalGrant{tool, input, identity, ttl, replay_max}, now)`,
     stores the resulting token, sets `ctx.approval_token = &token`,
     and re-dispatches. The audit row's `outcome` then transitions
     from `ask` to `approved`.
  4. On `Verdict::ask` rejection (e.g. `reason=replay_exhausted`), the
     loop drops its cached token, re-prompts, re-approves, and tries
     again — bounded by a sensible retry cap. The rejection audit
     row is already on disk for forensics.
  - The render-side flow that asks the operator (`permission_ask_rendered`)
    and captures the response (`permission_ask_resolved`) is hook-bus
    territory; the bus does not yet exist. The interim agent-loop
    slice can prototype an in-process callback that the hook bus
    replaces later without touching the dispatch surface.

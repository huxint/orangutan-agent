## [2026-05-24 01:15] | Task: Bound approval broker grants per identity

### Execution Context

- Agent: `Codex`
- Base model: `GPT-5`
- Runtime: `Codex CLI`
- Linked plan: none - scoped completion of spec 0012's approval-grant
  bounded-state item.

### User Query

> Continue slice 56 and commit the broker bounded-state close-out.

### Changes Overview

- Areas: `oran-permission`, approval broker state, docs/status.
- Key actions:
  - Added `ApprovalBroker::max_grants_per_identity = 64`.
  - `ApprovalBroker::approve` now lazily reaps expired grants before
    enforcing the per-identity cap.
  - A new distinct `(tool, identity, input_hash)` grant beyond the cap
    evicts the oldest live grant for that identity only. Re-approving an
    existing triple overwrites the entry and keeps the grant count stable.
  - Added bounded-state regressions for oldest-entry eviction, per-identity
    isolation, and reap-before-cap behavior.
  - `xmake run orangutan` now reports `2.0.0-slice56`.

### Design Intent

Spec 0012 requires every cache-like process-local structure to have an
explicit bound before `oran-agent` starts running long-lived turns. Approval
grants already had TTL and explicit `reap_expired(now)`, but a busy identity
could still accumulate many distinct approvals between reap ticks. The broker
is the smallest owner of that state, so the cap belongs there rather than in
the future scheduler or agent loop.

The eviction result deliberately surfaces as the existing `reason=no_grant`.
The token can still verify cryptographically because the authority remains
stateless; what disappeared is the broker's replay grant. Keeping the reason
stable avoids inventing a second "missing grant" branch for callers and audit
consumers.

### Files Modified

- `include/oran/permission/approval_broker.hpp`
- `src/oran-permission/approval_broker.cpp`
- `tests/permission/test_approval_broker.cpp`
- `src/oran-bootstrap/bootstrap.cpp`

### Docs Updated In This PR (Prime Directive - see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` - slice 56, new history pointer, refreshed
  `oran-permission` test counts (86 / 390), and next-intended-slice
  narrative now treats the approval-grant bounded-state item as closed.
- `docs/ARCHITECTURE.md` - `oran-permission` inventory and slice-status
  block record the 64 live grants per identity cap.
- `docs/design-docs/permissions-and-hooks.md` - approval broker status
  records lazy reap + oldest same-identity eviction.
- `docs/product-specs/0008-permissions.md` - approval replay acceptance
  criterion records the cap and `reason=no_grant` behavior for evicted
  tokens.
- `docs/product-specs/0012-tool-scheduler-and-state.md` - bounded-state
  inventory marks approval broker grants as shipped.
- `docs/QUALITY_SCORE.md` - refreshed permission test counts and current
  permission state.
- `docs/releases/feature-release-notes.md` - user-visible release note.

### Validation

- Commands run:
  - `xmake build test-permission`
  - `xmake run test-permission`
- Tests added/changed:
  - `tests/permission/test_approval_broker.cpp` adds three
    `[bounded]` regressions.
  - Full `test-permission` reports 86 cases / 390 assertions.
- Bench impact: not measured. `approve` does an O(live grants) scan when
  issuing a grant; approval issuance is an operator-mediated path and not in
  the hot tool-dispatch loop. The existing `bench-permission/approval_broker`
  remains the baseline if this becomes visible.
- Compile-budget delta: not measured; the change adds no heavy public-header
  dependency.

### Follow-Ups

- Issues opened: none.
- Tech-debt entries: no new row. Remaining spec 0012 v1 work is the actual
  scheduler / path-lock implementation once `oran-agent` or the temporary
  `oran-tool::Scheduler` slice begins.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`

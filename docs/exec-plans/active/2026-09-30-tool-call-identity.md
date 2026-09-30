# Validate Tool-Call Identity Before Effects

## Objective

Reject ambiguous or replayed provider tool-call IDs before dispatch. Empty IDs
and duplicates currently flow through the loop and scheduler, so even a repeated
call can execute before the next provider request exposes invalid correlation.

## Complete Slice

- After response-completion checks, validate every tool-use ID in the entire
  response before adding transcript rows or admitting any tool effects.
- IDs must be nonempty and unique across generated tool batches in the current
  `run_turn`. Track owned IDs independently of the compacted provider view.
- A malformed batch fails the turn with a structured upstream error; normal
  provider accounting and terminal traces still run. No synthetic result can
  repair ambiguous correlation, and no automatic retry is introduced.
- Identical arguments with fresh IDs remain valid separate operations. Prior
  turns and sessions are outside this in-memory replay boundary. Earlier effects
  in the same turn remain committed under existing effect contracts.
- Do not change prompt text, schemas, permission policy, provider protocols,
  persistent data formats or the scheduler's public API.

## Verification

Prove the defect first with controlled-provider tests: a batch containing a valid
call before an empty/duplicate ID must have zero effects. Cover repeated IDs
across iterations, fresh-ID repeats, accounting/traces and failed-session
persistence. Run the affected tests, complete release build/test suite and
`make ci`, then inspect and commit. Existing borrowed async lifetimes do not change.

Owners are `docs/design-docs/agent-platform.md` and the existing provider/tool
contracts. Keep implementation local to the loop with an owned per-turn set;
no dependencies, public heavy includes or compile-budget changes are needed.
Delete this plan once verification and the owning contract update are complete.

## Progress

- [x] Inspect the response-to-dispatch path and identify missing ID admission.
- [ ] Reproduce and implement the guard.
- [ ] Verify integration and update the contract.

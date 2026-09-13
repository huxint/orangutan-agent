# Provider Outcomes And Current Contracts

## Goal

Make provider execution own target attribution and cost estimation. Remove
obsolete compatibility obligations and hand-maintained prompt versions under the
user's explicit authorization for breaking changes.

## Scope

- Record that active development replaces API/configuration/derived-cache
  contracts directly, removing obsolete branches, shims, fixtures and migration
  narratives. Actual user records remain subject to their storage contract.
- Delete `SectionVersions` and `LoopOptions::prompt_versions`. Fingerprint the
  joined system text and native declarations; count submitted system-text bytes,
  including separators. Old derived cache identities need no preservation.
- Narrow `provider::System::send` to one `ModelTarget`. Replace the retry
  decorator with an explicit execution function over a backend and `Route`.
- Return attribution alongside a `core::Result<Response>` for every terminal
  invocation, including failure. The result remains the single error channel;
  attribution is execution-owned, not inferred from response hints or error text.
- Move profile cost estimation into execution, preserve provider-supplied cost,
  and distinguish reported model names from the selected profile/protocol.
  Remove `Response::route_profile_used` and the loop's route searches, pricing
  functions and repeated attribution defaults. Bootstrap borrows the backend
  directly; all callers move to the current API without compatibility overloads.
- Preserve retry budgets, per-target thinking/cache policy, stream visibility,
  cancellation joins, hook/audit authority and transcript persistence.

## Boundaries And Risks

[Provider](../../design-docs/api-portability.md) owns invocation/attempt values,
[agent](../../design-docs/agent-platform.md) owns consumption of terminal outcomes,
and [prompt design](../../rules/prompt-design.md) owns current cache identity.
[Collaboration](../../REPO_COLLAB_GUIDE.md) owns the breaking-change policy.

Actual profile/protocol attribution must not be spoofed by backend response or
error metadata. Failure and cancellation need the target that was attempted;
missing provider model names use the configured target. Existing external
callers must adapt directly. No database, file-conflict token, required protocol
header or dependency-version mechanism is removed merely because it has a
version number. No stored data is deleted or rewritten by this slice.

This crosses public interfaces and controlled fixtures in multiple libraries and
exceeds the ordinary review-size guideline. New metadata uses owned values and
the existing result/error model. No new package, thread or cache owner is added;
release LTO and compile budgets remain unchanged.

## Verification And Completion

- [x] Trace current version, retry, pricing and attribution consumers.
- [ ] Remove manual prompt versions and obsolete compatibility guidance/tests.
- [ ] Implement one-target sends and execution-owned terminal outcomes.
- [ ] Migrate the loop, bootstrap, controlled providers and direct callers.
- [ ] Exercise primary/fallback success and failure, reported/missing model,
  supplied/estimated cost, per-target policy, partial streams and cancellation.
- [ ] Build release libraries, pass all 14 test targets and affected benchmarks;
  run `make ci`, inspect the diff and update owning contracts.
- [ ] Delete this plan and commit the complete implementation.

Use `xmake f -y -m release`, affected `test-prompt`, `test-provider` and
`test-agent` targets while iterating, then `xmake build -j4`, `xmake test -j4`,
affected `bench-*` builds and `make ci` for the complete gate. Cancellation work
also receives affected debug ASan/UBSan checks before restoring release mode.

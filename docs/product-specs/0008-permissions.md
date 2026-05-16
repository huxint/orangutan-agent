# 0008 — Permissions Engine

## User Problem

A runtime that can write files and run shell is a runtime that can destroy a machine.
Operators need fine-grained, audited control over what the agent may do, with explicit
human approval for high-risk operations.

## Scope (v1)

- `oran-permission::Evaluator` with modes: `auto`, `default`, `permissive`, `strict`,
  `sandboxed`.
- Rule types: `allow`, `deny`, `ask`.
- Runtime regex via `re2` (not compile-time ctre).
- Approval prompts HMAC-signed with a per-process secret; replay limited by TTL +
  count.
- Capability-aware gating (rules can match by `Capability`, not just tool name).
- Audit log in `audit.db` for every decision (allow / deny / ask / approved /
  rejected).

## Scope (v1.1)

- Per-channel permission overlays — restrict what the agent may do when a request
  arrived via specific channels (e.g. less trusted external webhook).
- Time-bound approvals — "allow for next 1 hour".
- Sticky approvals — "always allow this exact input shape".

## Scope (v2)

- Approval routing to external channels (Slack, email) for human sign-off.
- Role-based permissions for multi-user runtimes.

## Out Of Scope

- Per-process kernel sandboxing (seccomp, etc.) — relies on external sandbox tools.
- Cryptographic attestation of approvals across machines.

## Acceptance Criteria

1. A tool call whose input matches a `deny` rule returns
   `Error::permission_denied` and is recorded in audit.
2. A tool call whose input matches an `ask` rule renders an approval prompt; on
   approval, replay works within TTL for identical input.
3. Capability mismatch is enforced — a tool that didn't declare `Capability::network`
   cannot use it even if a rule otherwise allowed. **(Foundation landed
   2026-05-16: `Rule::capability` + capability-aware
   `RuleSet::evaluate` shipped in `oran-permission`. Runtime
   `Capability::network` does not exist yet; the spec text predates the
   final enum — see `core::Capability::egress_http` /
   `egress_websocket`.)**
4. `re2` patterns load from config; invalid patterns at load time are reported with
   line numbers. **(Foundation landed 2026-05-16: `oran-config` now
   parses the `permissions` root block and each
   `agents.<name>.permissions` overlay into typed
   `PermissionsConfig`, and `permission::materialize(Mode, global,
   per_agent) -> RuleSet` concatenates defaults + global + overlay
   into the runtime evaluator. Runtime regex landed 2026-05-17:
   `permission::InputPattern` wraps `re2::RE2` (partial match) and
   surfaces compile errors with the re2 message attached to
   `Error::invalid_argument`; `Rule::input_pattern` plumbs the
   pattern into the evaluator's four-argument
   `evaluate(tool, input, capabilities, mode)` overload.
   `oran-config` parsing of `input_pattern` from each rule, with
   the line-numbered error report, lands in the matching
   `oran-config` slice.)**
5. Approval signing key is rotated when the runtime restarts; prior approvals are
   invalidated.
6. `tests/permission/` ≥ 90% coverage including table-driven tests over modes ×
   rule kinds × capability flags.

## Design Doc Cross-References

- [`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)
- [`../design-docs/secrets-and-state.md`](../design-docs/secrets-and-state.md)

## Risks

- Misconfigured rules silently broaden permissions — schema validation + a
  "rule explain" CLI subcommand mitigate.
- Approval prompts grow stale and annoying — TTL + sticky approvals address this.

## Validation

```sh
xmake build oran-permission
xmake test test-permission
xmake run orangutan -- --explain-rules
```

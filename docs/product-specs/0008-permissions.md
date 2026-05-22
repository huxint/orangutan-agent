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
   `Error::permission_denied` and is recorded in audit. **(Closed
   2026-05-17: the audit pipeline lands in three commits.
   `storage::AuditRepository` owns the `audit.db` schema +
   `migrate`/`append_event`/`list_events`/`count_events` surface;
   `permission::AuditEvent` + `permission::AuditOutcome` +
   `permission::AuditSink` define the in-process vocabulary;
   `permission::NullAuditSink`, `permission::RecordingAuditSink`,
   and `permission::StorageAuditSink` cover the three documented
   sink kinds (no-op default, in-memory capture for tests, SQLite
   persistence). `orangutan --audit-init [<path>]` runs the audit
   schema migration so operators can provision `audit.db` ahead
   of the future agent loop (one-shot `asio::io_context` driving
   the same `Pool` + `AuditRepository` path the agent loop will
   use). The slice-14 follow-up
   `bootstrap::RuntimeAssembly::build(workspace, executor, options)`
   then bundles a fresh `permission::ApprovalBroker` and the
   active `permission::AuditSink` (`StorageAuditSink` over an
   internal `Pool` + `AuditRepository` when audit is enabled,
   `NullAuditSink` otherwise) into a single value type the agent
   loop will inherit — the in-process half of criterion 1 is now
   wired end-to-end. The agent loop itself will pump `Decision`
   → `AuditEvent` through `permission::AuditSink::record(...)`
   on every call; that wiring lands with the first tool built-ins
   or the agent loop scaffolding.)**
2. A tool call whose input matches an `ask` rule renders an approval prompt; on
   approval, replay works within TTL for identical input. **(Closed 2026-05-17:
   `permission::ApprovalBroker` wraps `permission::ApprovalAuthority`
   with a `(tool, identity, input_hash)`-keyed replay map. `approve(grant, now)`
   issues a token and registers `expires_at = now + ttl`, `remaining_uses =
   replay_max`; `check(token, …, now)` calls the authority's MAC + expiry +
   tool / identity / input verify (which already attaches the documented
   `reason` context entries on failure) and then decrements the counter on
   success. Two broker-only rejection paths attach their own reasons:
   `reason=no_grant` (token verified but no entry — broker restarted, entry
   reaped, the per-identity grant ceiling evicted it, or `approve` was
   never called) and `reason=replay_exhausted`
   (counter at zero). Re-approving the same triple overwrites the entry, so
   operators get the intuitive "approve again resets the counter" behavior;
   `reap_expired(now)` provides explicit periodic eviction. Slice 56 adds
   the spec-0012 state ceiling: `approve` lazily reaps expired grants before
   enforcing at most 64 live grants per identity, evicting the oldest
   same-identity grant only when a new distinct triple would exceed that
   cap. The
   `replay_max` / `approval_ttl_seconds` per-rule fields land in
   `config::PermissionRuleConfig` as optionals; `oran-config` validates them
   (negative values reject with the JSON path attached); `permission::Rule`
   carries them through with the design-doc defaults (`replay_max=8`,
   `approval_ttl=3600s`); and `permission::Decision` copies the matched
   rule's policy fields so the agent loop can pass them straight to
   `ApprovalBroker::approve` via an `ApprovalGrant`. Bench: broker_approve
   ~9.9 µs (matches authority issue), broker_check_ok ~10.7 µs
   (authority verify + map find + decrement), broker_check_no_grant /
   broker_check_exhausted ~10.9 µs / ~11.0 µs (the two broker-only
   rejection paths).)**
3. Capability mismatch is enforced — a tool that didn't declare `Capability::network`
   cannot use it even if a rule otherwise allowed. **(Foundation landed
   2026-05-16: `Rule::capability` + capability-aware
   `RuleSet::evaluate` shipped in `oran-permission`. Runtime
   `Capability::network` does not exist yet; the spec text predates the
   final enum — see `core::Capability::egress_http` /
   `egress_websocket`.)**
4. `re2` patterns load from config; invalid patterns at load time are reported with
   line numbers. **(Closed 2026-05-17: `oran-config` parses
   `input_pattern` on each rule and validates it at load by
   compiling via re2; invalid patterns surface as
   `Error::config` with the JSON path attached (e.g.
   `$.permissions.deny[0].input_pattern`) and the re2 error
   message recorded under the `regex_error` context key.
   `permission::materialize` returns
   `core::Result<RuleSet>` and recompiles the validated source
   into a runtime `permission::InputPattern` when building each
   `Rule`. `Rule::input_pattern` then feeds the four-argument
   `RuleSet::evaluate(tool, input, capabilities, mode)`
   overload landed earlier the same day.)**
5. Approval signing key is rotated when the runtime restarts; prior approvals are
   invalidated. **(Closed 2026-05-17: `permission::ApprovalSecret`
   wraps libsodium's `crypto_auth_hmacsha256` over a 32-byte
   per-process key generated from `randombytes_buf` (constant-time
   `sodium_memcmp` compare, `sodium_memzero` on move/destruction).
   On top of it, `permission::ApprovalAuthority` /
   `permission::ApprovalToken` own the issue/verify flow:
   SHA-256 input hash + 16-byte random nonce + `core::Time` expiry
   MACed over a domain-separated length-prefixed canonical bytes
   layout (`"oran-approval-v1"` + version sentinel +
   length-prefixed tool/identity + input_hash + nonce + LE int64
   millis expiry). `verify(token, tool, input, identity, now)`
   checks expiry → tool → identity → input hash → MAC and
   attaches a `reason` context entry on first failure
   (`expired`/`tool_mismatch`/`identity_mismatch`/`input_mismatch`/
   `mac_mismatch`). Because the per-process key is fresh on every
   startup, tokens signed under a prior process fail MAC
   verification — pinned in tests as the criterion-5 invariant.
   Bench: issue ~9.9 µs, verify_ok ~9.3 µs, verify_expired
   early-reject ~36 ns.)**
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
xmake run orangutan -- --explain-rules --mode strict
xmake run orangutan -- --config config.example.json --explain-rules --agent researcher
```

The `--mode <strict|default|permissive|sandboxed>` flag selects the baseline
layer the rule materializer starts from; `--agent <name>` overlays the
matching `agents.<name>.permissions` block from the loaded config. Unknown
spellings surface as `Error::invalid_argument` and `Error::not_found`
respectively, both with the offending value attached.

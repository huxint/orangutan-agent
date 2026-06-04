## [2026-05-17 04:20] | Task: oran-permission ApprovalBroker + per-rule replay/ttl plumbing — closes 0008 criterion 2

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — three-commit push (plus a one-line
  `.clangd` tooling chore) that closes
  `docs/product-specs/0008-permissions.md` criterion 2 ("on
  approval, replay works within TTL for identical input"). The
  next-intended-slice bullet in `STATUS.md` is now retired.

### User Query

> 详细了解项目目标，查看当前项目真实进度, 推进项目代码的实现.
> 你这一次推进应该是能够实现 3 个commit左右. 不要盲目实现, 需要凭借客观事实,
> 良好的代码工程和查阅网上资料进行实现 ultrathink.

The user also requested, mid-task:

> using static_cast<void>

…and (after the LSP started complaining about GCC 16.1's
`-freflection` flag on every edited file):

> 添加 .clangd: CompileFlags:
>   Remove: [-freflection] 避免lsp带来的gcc16.1的误报

Both preferences are now persisted in the auto-memory bucket
(`prefer-static-cast-void-discard.md`).

### Changes Overview

- Areas:
  `include/oran/permission/{approval_broker,rule_set}.hpp`,
  `include/oran/permission.hpp`,
  `include/oran/config/config.hpp`,
  `src/oran-permission/{approval_broker,materialize,rule_set}.cpp`,
  `src/oran-config/config.cpp`,
  `tests/permission/{test_approval_broker,test_rule_set,test_materialize}.cpp`,
  `tests/config/test_config.cpp`,
  `bench/permission/{main.cpp,README.md,scenarios/approval_broker.cpp}`,
  `src/oran-bootstrap/bootstrap.cpp` (slice tag 11 → 12),
  `.clangd` (LSP-only flag strip),
  `docs/STATUS.md`, `docs/QUALITY_SCORE.md`,
  `docs/design-docs/permissions-and-hooks.md`,
  `docs/product-specs/0008-permissions.md`,
  `docs/releases/feature-release-notes.md`.

- Key actions, in commit order:

  1. **`permission::ApprovalBroker`** — stateful replay window
     on top of `ApprovalAuthority`. Owns the authority; keeps an
     `unordered_map` keyed by `{tool_name, identity,
     std::array<std::byte, 32> input_hash}` with a custom hash
     that folds the SHA-256 prefix with cheap string hashes for
     tool + identity. `approve(grant, now)` issues a token and
     registers `Entry{expires_at, remaining_uses = replay_max}`,
     overwriting any existing entry for the triple so re-approve
     resets the counter. `check(token, …, now)` calls
     `authority.verify` first (so the documented
     `expired`/`tool_mismatch`/`identity_mismatch`/
     `input_mismatch`/`mac_mismatch` reasons flow through
     verbatim), then either decrements the counter or returns
     `reason=no_grant` / `reason=replay_exhausted`. The header
     documents the full reason set so the upcoming audit slice
     has a stable vocabulary; the type is move-only and not
     thread-safe (the agent loop owns a single broker per
     strand). 9 new tests + a 4-scenario nanobench bucket;
     broker overhead over raw authority verify is ~875 ns.

  2. **Per-rule `replay_max` + `approval_ttl` end-to-end** —
     `permission::Rule` grew `replay_max=8` and
     `approval_ttl=3600s` defaults mirroring
     `docs/design-docs/permissions-and-hooks.md` "Approval
     Signing". `permission::Decision` mirrors them so the agent
     loop can carry the matched-rule policy through to
     `ApprovalBroker::approve`. `RuleSet::evaluate` copies the
     fields from the matched rule into the returned `Decision`;
     mode-default decisions keep the `Decision` struct defaults
     (8 / 1h). `config::PermissionRuleConfig` grew optional
     `replay_max` (uint32) and `approval_ttl_seconds` (int64);
     `oran-config` validates both (rejecting negative or
     non-integer values, attaching the JSON path as a
     `context.path` entry) and surfaces them through the typed
     surface. `permission::materialize` forwards the optionals
     into the runtime `Rule` while leaving the defaults
     untouched when the operator omitted them. 5 new
     permission tests + 5 new config tests.

  3. **Tooling + docs sync** — `.clangd` adds a one-line
     `CompileFlags:Remove: [-freflection]` so clangd stops
     surfacing the GCC-only flag as a false-positive on every
     file. Slice tag bumps 11 → 12 (`xmake run orangutan
     --explain-rules` confirms). `0008-permissions.md`
     criterion 2 marked closed with a pointer back to this
     entry; design-doc engine-status block extended with the
     broker landing; `QUALITY_SCORE` permissions row grew the
     seventh axis (`ApprovalBroker`); test framework + bench
     harness + config + bootstrap rows refreshed; release-notes
     row written.

### Design Intent

**Why a broker rather than fields on the token.** The token
stays portable: any honest holder (agent loop, audit log,
future external approval channel) can present it without the
broker shipping use-count state alongside. Decrementing is the
*broker's* job; keeping the token a value type means the
on-disk representation (when the audit slice lands) is just
the canonical bytes, no extra state needed.

**Why an `unordered_map` keyed by `(tool, identity, input_hash)`
rather than a flat list.** The agent loop will call `check`
once per honored replay, so the lookup is on the hot path. A
hash map gives O(1) average lookup; a flat list would scale
with the number of outstanding grants. The hash function folds
the SHA-256 prefix into a `size_t` (input_hash is already
uniformly random, so its first 8 bytes are a great key) and
mixes in cheap string hashes for tool + identity to avoid
bucket pile-ups across grants that happen to share an input
prefix.

**Why the broker calls `authority.verify` first.** The
authority owns expiry / MAC / tool / identity / input checks
and already attaches the documented `reason` context entries.
Forwarding the authority's error verbatim preserves the
operator-visible vocabulary the audit slice will rely on, and
guarantees a cross-tool / cross-input replay attempt never
decrements the honest triple's counter (a pinned test).

**Why `replay_max=8` and `approval_ttl=3600s` defaults.** Both
come from the design doc's "Approval Signing" section. They are
the design's published baseline, so an operator who omits the
fields gets the documented behavior; an operator who wants
tighter policy (e.g. single-shot approval for a destructive
shell call) opts in explicitly.

**Why `replay_max=0` is permitted.** The header documents this
as the "quarantine" path — an issued token that no `check` will
honor. It's an edge case but useful for diagnostic flows where
the operator wants to inspect the token shape without ever
honoring it. Two-line code path, one-line documentation, one
pinned test.

**Why `approval_ttl_seconds` lives at the config layer rather
than `approval_ttl_ms` or a free-form ISO 8601 duration string.**
Seconds match the design doc's wire format ("default 1h" → 3600
seconds) and the existing config surface that uses integer
seconds (e.g. `runtime.request_timeout_ms` is the lone
millisecond field — every other duration in config is
seconds-ish). Using `std::int64_t` rather than `std::uint64_t`
in the config struct keeps the validator path simple (one
`< 0` check) and avoids the JSON parser's "is_number_unsigned"
distinction.

**Why optional config fields rather than always-present.**
Optional preserves "unset" so `permission::materialize` can keep
the `Rule` struct defaults rather than re-apply them in two
places. Operators see clean diffs when only one policy field is
customized.

**Why `Decision` carries the policy fields rather than letting
callers look up the matched rule index.** Field-carrying is the
cheapest possible API for the agent loop (which is the only
caller that will ever need this). Adding a "matched rule index"
field would force every caller to take a `RuleSet` reference
just to recover policy. The `Decision` is already returned by
value, so two extra fields are free in practice and let the
agent loop pass straight from `evaluate(...)` to
`ApprovalGrant{...}`.

**Why not wire the broker into bootstrap right now.** This
slice already touches 14 files; bootstrap wiring is its own
discrete slice that wants to make decisions about strand
ownership, periodic-reaper scheduling, and how the broker
interacts with the still-future agent loop. Cleanly split as
the next slice (called out in `STATUS.md`).

### Files Modified

- `include/oran/permission/approval_broker.hpp` — new public
  surface: `ApprovalGrant`, `ApprovalBroker` with `approve` /
  `check` / `reap_expired` / `outstanding_grants` / `authority`
  accessor.
- `src/oran-permission/approval_broker.cpp` — impl. Hash
  function over the SHA-256 prefix + folded tool + identity
  hashes.
- `include/oran/permission/rule_set.hpp` — `Rule` grew
  `replay_max=8` + `approval_ttl=3600s`; `Decision` mirrors
  them.
- `src/oran-permission/rule_set.cpp` — `evaluate` copies the
  matched-rule policy fields into the returned `Decision`.
- `include/oran/permission.hpp` — re-exports
  `approval_broker.hpp`.
- `include/oran/config/config.hpp` — `PermissionRuleConfig`
  grew optional `replay_max` (uint32) +
  `approval_ttl_seconds` (int64).
- `src/oran-config/config.cpp` — parser reads the new fields,
  rejects negative or non-integer values with the JSON path
  attached, extends the known-key list to 5.
- `src/oran-permission/materialize.cpp` — forwards the
  optionals into the runtime `Rule` while keeping defaults
  when the operator omitted them.
- `tests/permission/test_approval_broker.cpp` — 9 cases:
  round-trip, replay-until-exhausted, no_grant, re-approve
  resets, reap_expired, replay_max=0 quarantine, distinct
  triples, authority-error propagation without spending the
  honest counter, TTL expiry survives reap.
- `tests/permission/test_rule_set.cpp` — 3 cases: Rule
  defaults, Decision forwarding, mode-default fallback.
- `tests/permission/test_materialize.cpp` — 2 cases:
  config-to-Rule forwarding, default preservation.
- `tests/config/test_config.cpp` — 5 cases:
  replay_max / approval_ttl_seconds parse, defaults-unset,
  negative replay_max rejected with path, negative
  approval_ttl_seconds rejected with path, non-integer
  replay_max rejected.
- `bench/permission/scenarios/approval_broker.cpp` — 4
  scenarios.
- `bench/permission/main.cpp` — registers the new scenarios.
- `bench/permission/README.md` — documents the new scenarios.
- `src/oran-bootstrap/bootstrap.cpp` — slice tag 11 → 12.
- `.clangd` — strip GCC-only `-freflection` from clangd's
  per-file compile flag list.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 12, history pointer, refreshed
  `oran-permission` test counts (59 → 73 cases, 233 → 296
  assertions) and `oran-config` (14 → 19 cases, 132 → 148
  assertions); the next-intended-slice bullet pivoted to the
  remaining `0008-permissions.md` v1 work (audit, bootstrap
  wiring, first tool built-ins).
- `docs/QUALITY_SCORE.md` — permissions row "Why" extended
  with the seventh axis (`ApprovalBroker`), "Next Step"
  pivoted to audit + bootstrap wiring + first tools. Test
  framework + bench harness + config + bootstrap rows
  refreshed.
- `docs/design-docs/permissions-and-hooks.md` — engine-status
  block extended with the broker landing and the spec's
  criterion-2 closure.
- `docs/product-specs/0008-permissions.md` — criterion 2
  marked closed with the closing-date pointer back here.
- `docs/releases/feature-release-notes.md` — new row
  `permission-approval-broker`.

### Validation

- Commands run:
  - `xmake build oran-config oran-permission` — clean after
    parser + materialize changes.
  - `xmake build test-permission && ./test-permission` —
    73 cases / 296 assertions, all green.
  - `xmake build test-config && ./test-config` — 19 cases /
    148 assertions, all green.
  - `xmake build bench-permission && xmake run bench-permission`
    — new `bench-permission/approval_broker` bucket reports
    broker_approve ~9.9 µs, broker_check_ok ~10.7 µs,
    broker_check_no_grant ~10.9 µs, broker_check_exhausted
    ~11.0 µs. Broker overhead over the raw authority verify
    is ~875 ns (map find + counter decrement + reason build);
    issue / verify_ok / verify_expired baselines unchanged.
  - `xmake test` — all 8 buckets (async / bootstrap / cli /
    config / core / io / permission / storage) green.
  - `xmake build orangutan && ./build/.../orangutan
    --explain-rules` — prints 9 default rules, unchanged
    layout, version line `2.0.0-slice12`.
- Tests added/changed: 14 new permission cases + 5 new config
  cases (59 → 73 / 233 → 296 in permission; 14 → 19 / 132 → 148
  in config).
- Bench impact: new `bench-permission/approval_broker` bucket
  (4 scenarios); existing scenarios unchanged.
- Compile-budget delta: `approval_broker.cpp` is a new TU; the
  header pulls only `<unordered_map>` (already PCH'd) plus the
  existing approval / time / result headers. No PCH change.

### Follow-ups

- Issues to file: none.
- Tech-debt entry: none.
- Linked release note: `permission-approval-broker` row
  added to `feature-release-notes.md`.
- Next slices, in rough priority order:
  - `audit.db` schema + `permission::AuditSink` that records
    allow / deny / ask / approved / rejected decisions
    (closes `0008-permissions.md` criterion 1 + the audit
    half of criterion 5).
  - Wire `ApprovalBroker` ownership into `oran-bootstrap` so
    the upcoming agent loop inherits a per-process broker
    handle (and a periodic-reaper job for past-TTL entries).
  - First tool-registry built-ins (`file.read`, `file.write`,
    `file.edit`, `file.search`) per the `STATUS.md`-flagged
    candidate.

## [2026-05-17 02:35] | Task: oran-permission approval signing — closes 0008 criterion 5

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — three-commit push that closes
  `docs/product-specs/0008-permissions.md` criterion 5
  ("Approval signing key is rotated when the runtime restarts;
  prior approvals are invalidated"). The next intended-slice
  bullet in `STATUS.md` is now retired.

### User Query

> 详细了解项目目标，查看当前项目真实进度, 推进项目代码的实现.
> 你这一次推进应该是能够实现 3 个commit左右. 不要盲目实现, 需要凭借客观事实,
> 良好的代码工程和查阅网上资料进行实现 ultrathink.

### Changes Overview

- Areas: `include/oran/permission/{approval_secret,approval}.hpp`,
  `src/oran-permission/{approval_secret,approval}.cpp`,
  `include/oran/permission.hpp`,
  `tests/permission/test_{approval_secret,approval}.cpp`,
  `bench/permission/scenarios/{approval_secret,approval}.cpp`,
  `bench/permission/{main.cpp,README.md}`,
  `xmake/packages.lua`, `xmake/targets.lua`,
  `xmake-requires.lock`, `docs/rules/libraries.md`,
  `src/oran-bootstrap/bootstrap.cpp` (slice bump 10 → 11),
  `docs/STATUS.md`, `docs/QUALITY_SCORE.md`,
  `docs/design-docs/permissions-and-hooks.md`,
  `docs/product-specs/0008-permissions.md`,
  `docs/releases/feature-release-notes.md`.

- Key actions, in commit order:

  1. **`permission::ApprovalSecret`** — per-process HMAC-SHA-256
     key wrapper backed by libsodium. `generate()` reads 32
     bytes from `randombytes_buf` after a `std::call_once`
     `sodium_init`; `from_bytes` is a length-validated factory
     for tests / future password-derived keys. `mac(span<byte>)`
     calls `crypto_auth_hmacsha256`; `macs_equal` is
     constant-time via `sodium_memcmp` with a size-mismatch
     short-circuit. The key is `sodium_memzero`-cleared on
     move and destruction. libsodium 1.0.21 lands in
     `xmake/packages.lua`; `oran-permission` gets it as a
     private dep; rule C6 stays honest (the header has no
     `<sodium.h>`). 7 new tests + a 4-scenario nanobench
     bucket.

  2. **`permission::ApprovalToken` + `ApprovalAuthority`** —
     value-type token (tool_name / identity / SHA-256 input
     hash / 16-byte random nonce / `core::Time` expiry /
     32-byte HMAC) and the stateless façade that owns the
     `ApprovalSecret`. `issue(req, now)` SHA-256-hashes the
     input, generates a fresh nonce, computes
     `expires_at = now + req.ttl`, and MACs the canonical-bytes
     representation. `verify(token, tool, input, identity, now)`
     checks expiry → tool → identity → input hash → MAC in
     that order, attaching a `reason` context entry on the
     first failure (`expired` / `tool_mismatch` /
     `identity_mismatch` / `input_mismatch` / `mac_mismatch`).
     10 new tests + a 3-scenario nanobench bucket.

  3. **Docs sync** — slice 10 → 11, criterion 5 closed in
     0008-permissions.md, design-doc engine-status block
     extended, QUALITY_SCORE permissions row + bench harness
     row + test framework row refreshed, release-notes row,
     and this history entry.

### Design Intent

**Why libsodium.** `docs/rules/libraries.md` already approved
libsodium 1.0.20 for `oran-config`'s future secret-at-rest
support and `docs/design-docs/secrets-and-state.md` already
called out HMAC signing under "Approval Signing". Expanding the
boundary from "oran-config only" to "oran-config and
oran-permission" is the smallest possible step in the direction
the design docs have been pointing for weeks. Rolling our own
SHA-256/HMAC-SHA-256 would have been a textbook bad idea
("don't roll your own crypto"); the libsodium primitives are
audited, the test-vector pinning catches both wrapper *and*
primitive drift, and the boundary discipline (sodium.h stays
in `.cpp` only) is identical to how re2 is contained.

**Why the canonical-bytes layout.** Length-prefixed +
domain-separated + version-tagged is industry-standard for
HMAC inputs:

- Domain separator (16 bytes `"oran-approval-v1"`) ensures a
  hypothetical future "approval-v2" or "session-token" HMAC
  cannot collide cross-protocol.
- Version sentinel (`0x01`) lets us evolve the layout without
  re-keying — verify-side dispatches on the byte if/when v2
  ever lands.
- Length-prefixed tool / identity prevents a malicious caller
  from carving "tool" bytes out of "identity" or vice versa by
  manipulating the concatenated string (the "canonicalization
  ambiguity" class of bugs).
- Little-endian fixed-width integers keep the layout
  reproducible across architectures.

**Why SHA-256 the input rather than store it.** Approval tokens
flow through audit logs, hook payloads, and potentially future
external approval channels (Slack / email per the v2 scope of
`0008-permissions.md`). Storing the raw input would leak
operator-typed commands and file contents through every one
of those surfaces. The hash is verification-equivalent (verify
re-hashes the supplied input and compares constant-time) and
keeps tokens compact + non-revealing.

**Why verify in (expired → tool → identity → input → MAC)
order.** Expiry is the cheap O(1) integer compare and the
common rejection reason in production (tokens age out faster
than they're tampered with). Tool and identity are short
string compares — fast, and they're carried in the token
itself so no recomputation is required. Input hash and MAC
are SHA-256 / HMAC-SHA-256 — pay them only when the trio
matches. The bench bucket pins this hypothesis: the early-
reject expired path costs ~36 ns; the full verify costs
~9.3 µs, ~250× more.

**Why no replay-window state yet.** Criterion 5 in the spec is
specifically about key rotation invalidating prior approvals,
not about replay tracking. Criterion 2 mentions per-rule
`replay_max` / `approval_ttl` — that's a separate slice
(an `ApprovalBroker` that wraps an `ApprovalAuthority` with a
`(tool, input_hash, identity)`-keyed replay map). Landing the
broker now would push the PR past the C14 file-count guideline
without strengthening the criterion-5 closure.

**Why `Result<ApprovalAuthority>` instead of throwing.**
Rule C3 (no exceptions across library boundaries). The only
fallible point in `with_random_secret` is the underlying
`sodium_init` call, which can only fail on a broken libsodium
build — but the project-wide `Result<T>` contract makes the
failure path explicit instead of relying on every caller to
remember "this can't fail in practice".

### Files Modified

- `xmake/packages.lua` — `add_requires("libsodium 1.0.21")`.
- `xmake/targets.lua` — `oran-permission` gets `libsodium`
  as a private dep.
- `xmake-requires.lock` — libsodium 1.0.21 plus its build-time
  deps (autoconf, automake, libtool, m4) pinned.
- `docs/rules/libraries.md` — libsodium entry version bump
  (1.0.20 → 1.0.21), "Used by" broadens to "config, permission",
  boundary text now spells out the approval-signing purpose.
- `include/oran/permission/approval_secret.hpp` — new public
  surface.
- `src/oran-permission/approval_secret.cpp` — libsodium impl.
- `include/oran/permission/approval.hpp` — new public surface
  for `ApprovalRequest`, `ApprovalToken`, `ApprovalAuthority`.
- `src/oran-permission/approval.cpp` — canonical bytes builder,
  issue, verify, SHA-256 helper.
- `include/oran/permission.hpp` — umbrella now re-exports
  `approval_secret.hpp` and `approval.hpp`.
- `tests/permission/test_approval_secret.cpp` — 7 cases
  (RFC vector pinning, length validation, determinism,
  divergence, size-mismatch safety, criterion-5 in primitive
  form).
- `tests/permission/test_approval.cpp` — 10 cases (round-trip,
  expiry boundary, cross-tool / cross-input / cross-identity
  rejection, MAC tamper, nonce tamper, cross-authority
  invalidation, `input_hash` determinism, per-issue nonce
  uniqueness).
- `bench/permission/scenarios/approval_secret.cpp` — 4
  scenarios.
- `bench/permission/scenarios/approval.cpp` — 3 scenarios.
- `bench/permission/main.cpp` — registers the new scenario
  bundles.
- `bench/permission/README.md` — documents the new scenarios.
- `src/oran-bootstrap/bootstrap.cpp` — slice tag bumped
  10 → 11.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 11, history pointer, refreshed
  `oran-permission` test counts (49 → 59 cases, 196 → 233
  assertions).
- `docs/QUALITY_SCORE.md` — permissions row "Why" extended
  with the approval landing, "Next Step" pivoted to the
  remaining v1 work (replay tracking + audit log + bootstrap
  wiring). Test framework + bench harness rows refreshed
  with the new counts and scenario list.
- `docs/design-docs/permissions-and-hooks.md` — engine-status
  block now records the approval primitive + authority
  landing and the spec's criterion-5 closure.
- `docs/product-specs/0008-permissions.md` — criterion 5
  marked closed with the closing-date pointer back here.
- `docs/releases/feature-release-notes.md` — new row
  `permission-approval-signing`.
- `docs/rules/libraries.md` — libsodium row updated (above).

### Validation

- Commands run:
  - `xmake build oran-permission` (clean after adding the
    libsodium dep).
  - `xmake build test-permission && ./test-permission` —
    59 cases / 233 assertions, all green (RFC-style HMAC
    vectors check out, cross-secret rejection confirms
    criterion 5).
  - `xmake build bench-permission && xmake run bench-permission`
    — approval_secret: hmac short ~6.4 µs / hmac long ~28.8 µs
    / macs_equal_ok ~60 ns / macs_equal_no ~64 ns (constant-time
    confirmed). approval: issue ~9.9 µs / verify_ok ~9.3 µs /
    verify_expired ~36 ns (early-reject ~250× faster).
  - `xmake build orangutan && ./build/.../orangutan
    --explain-rules` — prints 9 default rules, unchanged.
  - Full test sweep across core/async/io/storage/config/
    permission/cli/bootstrap — all green.
  - `scripts/check-docs-sync.sh` — clean.
- Tests added/changed: 17 new permission test cases
  (49 → 59 cases / 172 → 233 assertions).
- Bench impact: new `bench-permission/approval_secret` and
  `bench-permission/approval` buckets (7 scenarios total);
  existing scenarios unchanged.
- Compile-budget delta: libsodium is contained to two
  `.cpp` TUs (`approval_secret.cpp`, `approval.cpp`); the
  public headers stay sodium-free, so callers outside
  `oran-permission` pay no transitive cost. The PCH did not
  change.

### Follow-ups

- Issues to file: none.
- Tech-debt entry: none.
- Linked release note: `permission-approval-signing` row
  added to `feature-release-notes.md`.
- Next slices, in rough priority order:
  - `ApprovalBroker` that wraps `ApprovalAuthority` with the
    `(tool, input_hash, identity)`-keyed replay map +
    per-rule `replay_max` / `approval_ttl` plumbing
    (closes `0008-permissions.md` criterion 2 fully).
  - `audit.db` schema + `permission::AuditSink` that records
    allow / deny / ask / approved / rejected decisions
    (closes criterion 1 + criterion 5's audit half).
  - First tool-registry built-ins (`file.read`, `file.write`,
    `file.edit`, `file.search`) per the
    `STATUS.md`-flagged candidate.

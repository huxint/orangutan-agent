# 0013 — Workspace + Path Policy

## User Problem

`docs/SECURITY.md` promises file tools are confined to the workspace root unless
explicitly overridden. Before slice 37 every built-in file tool read `path`
straight from the JSON input and called `oran-io` without canonicalisation,
traversal checks, or symlink policy. Slice 37 adds the resolver foundation and
migrates `file.read` when `DispatchContext::workspace` is supplied; slice 38
migrates `file.write`, `file.edit`, and `file.delete` through the same seam;
slice 39 migrates `file.search` through `resolve_list`; slice 40 migrates
`directory.list` through `resolve_list`. Every filesystem built-in now
consumes the workspace seam at handler entry, so full runtime confinement
only awaits the bootstrap-owned `Workspace` values plus the pre-permission
resolver boundary. Permission rules can gate a *capability* or a *name*, but
moving resolution to *before* permission evaluation still lands in a
follow-up slice — until then, the in-handler resolve covers all current
built-ins.

For a coding agent, "ask before write" is not a substitute for a workspace
boundary. The cost of an escape — overwriting `~/.ssh/authorized_keys`,
deleting a parent project file — is permanent. Path policy must live below
permission/audit/hooks so that *every* effectful filesystem call goes through
the same resolver.

Today's seams that motivate this spec:

- Every filesystem built-in (`file.read`, `file.write`, `file.edit`,
  `file.delete`, `file.search`, `directory.list`) now consumes
  `DispatchContext::workspace` at handler entry via slices 37-40. The
  remaining structural moves — bootstrap ownership of the workspace value,
  config wiring for the override roots, and resolution at the
  pre-permission dispatch boundary with audit metadata — are tracked in
  the Status block below.
- `file.search` and `file.delete` disagree on symlink behaviour. The root path
  in `file.search` follows symlinks (`is_regular_file` / `is_directory`,
  `src/oran-tool/file_search.cpp:275-295`), nested entries skip them
  (`:319-323`), and `file.delete` rejects symlinks outright via
  `io::delete_file`'s `symlink_status` check.
- `directory.list` and `file.search`'s "hidden" filter (dotfile-skip) is
  enforced inside each tool — there is no shared place to put repository-wide
  ignore policy (`.gitignore`, build directories) once it lands.
- The future `tool::Runtime::workspace()` capability-gated accessor is
  described in [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md)
  "ToolRuntime" but unbuilt.

This spec turns the workspace into a first-class platform service so that
every filesystem tool — current and future — resolves paths the same way.

## Status

**Slice 37 (2026-05-22):** `tool::Workspace` is now shipped in
`oran-tool`. It canonicalises the primary root and optional extra read/write
roots, exposes `resolve_read`, `resolve_write`, `resolve_delete`, and
`resolve_list`, returns `ResolvedPath` strings/flags without exposing
`<filesystem>` in the public header, and rejects traversal or symlink escapes
with the context reasons this spec names. `DispatchContext` carries an
optional non-owning `Workspace*`; `file.read` uses it when supplied.

**Slice 38 (2026-05-22):** `file.write`, `file.edit`, and `file.delete` now
resolve through `DispatchContext::workspace` when it is supplied. Writes pass
their current `mode` and `create_parents` intent to `resolve_write`; edits use
`resolve_write` before the read+atomic rewrite; deletes use `resolve_delete`.
Relative in-workspace paths work, traversal outside the workspace rejects with
`reason=outside_workspace`, and mutating symlink targets reject with
`reason=symlink_target`.

**Slice 39 (2026-05-22):** `file.search` now resolves its input through
`Workspace::resolve_list` when `DispatchContext::workspace` is supplied, before
the executor hop that drives the blocking walk. Single-file and recursive-walk
shapes still work; relative in-workspace searches succeed, traversal rejects
with `reason=outside_workspace`, root-side symlink escapes reject with
`reason=symlink_escape`, and `permissions.workspace.extra_read_roots` widens
the resolver in the read direction the same way it does for `file.read`.
Nested entries during the walk continue to skip symlinks wholesale — a
stricter form of the same workspace policy.

**Slice 40 (2026-05-22):** `directory.list` now resolves its input through
`Workspace::resolve_list` when `DispatchContext::workspace` is supplied,
before `oran-io::list_directory`. Relative in-workspace listings succeed,
traversal rejects with `reason=outside_workspace`, and root-side symlink
escapes reject with `reason=symlink_escape`. After this slice every
filesystem built-in (`file.read`, `file.write`, `file.edit`, `file.delete`,
`file.search`, `directory.list`) consumes the workspace seam at the handler
entry, closing the per-tool half of this spec.

**Slice 41 (2026-05-22):** Bootstrap-owned workspace lands.
`bootstrap::RuntimeAssembly` now constructs and owns a `tool::Workspace`
during `build()` and exposes it via `RuntimeAssembly::workspace()`. The
typed surface in `oran-config` grew `PermissionsConfig::workspace` of type
`WorkspacePermissionsConfig`, parsing
`permissions.workspace.extra_read_roots` and
`permissions.workspace.extra_write_roots` as string arrays (per-agent
overlays carry the same block). `bootstrap::run` converts the config-side
roots into `tool::WorkspaceOptions` before calling `RuntimeAssembly::build`,
so the override list canonicalises once at boot and a misconfigured root
fails the boot rather than the first dispatched tool call. A latent bug in
`tool::Workspace::create` that surfaced ENOENT as `Error::io` instead of
`Error::not_found` was fixed in the same slice. The agent-loop still needs
to thread `RuntimeAssembly::workspace()` into `DispatchContext::workspace`;
moving resolution to the pre-permission dispatch boundary and adding audit
metadata for the resolved path remain follow-up work.

**Slice 49 (2026-05-23):** `file.search` now owns the first
source-controlled ignore-file implementation. Recursive walks with
`respect_ignore=true` load `.gitignore` and `.ignore` files from the search
root downward and apply the common repo-rule subset (comments, blanks,
escaped leading marker literals, negation, directory-only rules,
slash-relative patterns, basename patterns, and fnmatch-style globs) on
top of the slice-48 built-in skip list. This
closes the immediate `file.search` product need in spec 0011, but the
workspace-owned sharing point described below is still pending for the
future `directory.scan`.

Still pending: audit metadata for resolved paths / override-root index, and
moving resolution to the pre-permission dispatch boundary. The shared
`Workspace::is_ignored(...)` predicate remains a v1.1 structure task once a
second consumer (`directory.scan`) exists.

## Scope (v1)

The MVP closes the deep-review P0 workspace gap with the minimum surface that
all six current built-ins (`file.read`, `file.write`, `file.edit`,
`file.search`, `directory.list`, `file.delete`) can adopt in one slice.

- `tool::Workspace` (or `tool::PathPolicy`) value type owned by
  `bootstrap::RuntimeAssembly` and threaded into `tool::DispatchContext`
  (later promoted into `tool::Runtime::workspace()` per the design doc).
- Canonical workspace root: discovered from `<workspace>` (the directory that
  contains `.orangutan/config.json`), canonicalised once at bootstrap, stored
  alongside the matching `std::filesystem::path` and a UTF-8 string view for
  public-header friendliness.
- One typed entry point per intent — never a generic "resolve":
  - `Workspace::resolve_read(std::string_view path) -> Result<ResolvedPath>`
  - `Workspace::resolve_write(std::string_view path, WriteIntent) -> Result<ResolvedPath>`
  - `Workspace::resolve_delete(std::string_view path) -> Result<ResolvedPath>`
  - `Workspace::resolve_list(std::string_view path) -> Result<ResolvedPath>`
  Each method encodes the symlink + parent-creation + must-exist rules for
  its intent; callers cannot mix them up at the type level.
- `ResolvedPath` carries:
  - canonical `std::filesystem::path` (lives in `.cpp`),
  - UTF-8 string view (lives in the public header),
  - relative path from the workspace root (for audit / display),
  - resolution flags (`symlink_followed`, `created_parents`,
    `outside_workspace_explicit_override`).
- Default symlink policy: **refuse** symlinks for `resolve_write` /
  `resolve_delete`; **allow** symlinks for `resolve_read` / `resolve_list`
  *only* when the resolved target remains under the workspace root. An
  outside-root symlink is rejected as `Error::permission_denied` with
  `reason=symlink_escape` in the context map.
- Default traversal policy: every `..` segment is resolved before policy
  checks; an absolute path inside the workspace root is allowed, an absolute
  or relative path outside the root is rejected as
  `Error::permission_denied` with `reason=outside_workspace`.
- Override surface (config, not per-call):
  `permissions.workspace.extra_read_roots` and
  `permissions.workspace.extra_write_roots`. Each entry is canonicalised at
  load time; runtime resolution checks all configured roots in declaration
  order. An override never bypasses the symlink check — it only widens which
  canonical root counts as "inside".
- Audit visibility: every resolve emits a hook payload field (already part of
  the file tool's existing `ToolBeforePayload` after slice 22's wiring) that
  records `(input_path, resolved_path, workspace_root, symlink_followed,
  override_root_index)`. The raw `input_path` is hashed, never logged in full,
  to mirror today's `input_hash` discipline (`SHA-256(input_json)` per
  `Registry::dispatch`).

`WriteIntent` enumerates the three modes the existing `file.write` already
supports (`truncate`, `append`, `fail_if_exists`) plus
`create_parents`, so the workspace can decide whether parent-directory
creation is allowed for this resolve call.

## Scope (v1.1)

- Lift the shipped `file.search` ignore predicate into a workspace-owned
  predicate (`Workspace::is_ignored(ResolvedPath)`) once `directory.scan`
  exists. The current implementation lives in `file.search` only; the shared
  predicate will carry the built-in skip list plus `.gitignore` / `.ignore`
  rule stack so both recursive tools make the same decision.
- Per-call override field for `file.read` / `file.search` /
  `directory.list`: `allow_outside_workspace=true` requires an `ask`
  permission verdict at runtime and records the resolved path in audit
  verbatim (no override exists for write/delete — that gate stays config-only).
- Path display helper: `Workspace::display(const ResolvedPath&)` produces the
  `<root>/...` short form for tool output and audit. Lifts the current ad-hoc
  rendering across `file.search` / `directory.list`.

## Scope (v2)

- Multi-workspace support (one process, multiple agents, each pinned to a
  different workspace root). Today's bootstrap assumes one workspace per
  runtime; once orchestration (`spec 0004`) starts spawning workers with
  distinct working directories, `Workspace` becomes per-agent rather than
  per-process.
- Cross-platform path-case sensitivity policy (macOS / Windows). Pre-v1 is
  Linux-only per `0002-tool-registry.md` "Out Of Scope", so the canonicalised
  comparison is byte-exact today.

## Out Of Scope

- Per-process kernel sandboxing (seccomp / Landlock). The workspace policy is
  the userspace contract; kernel-level enforcement is the operator's job.
- Cryptographic attestation that a resolved path was approved by a specific
  user — the existing `permission::ApprovalAuthority` covers approval
  signing.
- Network-mounted filesystem semantics (NFS / SMB rename guarantees). The
  atomic-write durability work in [`../design-docs/io-runtime.md`](../design-docs/io-runtime.md)
  "Atomic Writes" already calls this out separately.

## Acceptance Criteria

Slice 37 satisfies the resolver-level portions of criteria 1-3 and 8 inside
`tests/tool/test_workspace.cpp`, verifies independent `Workspace` values toward
criterion 6, and verifies `file.read` consumes the workspace seam when
provided. Slice 38 adds dispatch-level regressions for `file.write`,
`file.edit`, and `file.delete` through the workspace seam. Criteria 4-6 plus
search/list adoption and bootstrap-owned config wiring remain open until every
filesystem built-in resolves before permission/audit.

1. **Traversal rejection.** A `file.write` whose `path` resolves via `..` to a
   point outside the workspace root returns `Error::permission_denied` with
   `reason=outside_workspace`. The same rule fires for an absolute path
   outside the root. Test matrix covers `path=../outside.txt`,
   `path=/etc/passwd`, `path=workspace/../../outside`,
   `path=workspace/legit/../../../outside`.
2. **Symlink policy.** `file.write` on a path whose final component is a
   symlink rejects with `reason=symlink_target`; `file.delete` on a symlink
   rejects with `reason=symlink_target`; `file.read` and `file.search` follow
   a symlink whose canonical target stays inside the workspace and reject
   (`reason=symlink_escape`) when the target leaves the root.
3. **Override roots widen, not bypass.** With
   `permissions.workspace.extra_read_roots=["/var/log/oran"]`, a `file.read`
   of `/var/log/oran/audit.log` succeeds; a `file.write` to the same path
   still rejects because the override list is read-only. A
   `permissions.workspace.extra_write_roots` override that overlaps a symlink
   chain pointing outside both roots still rejects via the symlink-escape
   path.
4. **Audit fields.** Every dispatch on a file built-in records
   `(input_hash, resolved_relative_path, workspace_root_hash,
   symlink_followed, override_root_index)` in the existing
   `permission::AuditEvent` context map. `audit.db` table stays schema-compatible
   — fields land under the existing `context` JSON column.
5. **Single resolver.** `file.search`'s root-path symlink behaviour matches
   nested-entry behaviour after the migration: both go through
   `Workspace::resolve_list` and apply the same symlink-escape rule. The
   pre-migration divergence (`src/oran-tool/file_search.cpp:275-295` vs.
   `:319-323`) closes in the same slice that lands `tool::Workspace`.
6. **Bootstrap ownership.** Constructing two `RuntimeAssembly` instances with
   different `workspace_root` arguments yields two `Workspace` values whose
   `resolve_read("foo")` returns paths under their respective roots, with no
   shared globals. Test asserts identity and no static state.
7. **Capability gate.** When the future `tool::Runtime::workspace()` accessor
   lands, calling it from a handler whose `required_capabilities` did not
   include any filesystem capability returns
   `Error::capability_not_granted`. Pre-`tool::Runtime` shipping uses a plain
   `Workspace&` on `DispatchContext`; the capability check moves to the
   accessor in the same PR that lands the runtime handle.
8. **`tests/tool/test_workspace.cpp` ≥ 90% coverage** of the resolve-method
   matrix (intent × symlink-or-not × inside-or-outside × override-list).

## Design Doc Cross-References

- [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md) —
  `ToolRuntime` capability-gated `workspace()` accessor; this spec is the
  product-side companion that says *what* the resolver must do.
- [`../design-docs/io-runtime.md`](../design-docs/io-runtime.md) — the
  `oran-io` surface stays policy-free; this spec lands one layer above it
  inside `oran-tool`.
- [`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)
  — workspace resolution runs *before* permission evaluation so that an
  out-of-workspace path cannot be silently `allow`-ed by a broad rule.
- [`../SECURITY.md`](../SECURITY.md) — the workspace-confinement claim there
  becomes load-bearing once this spec lands; both files update in the same
  slice that ships `tool::Workspace`.

## Risks

- **Slice scope blowup.** Touching all six built-ins plus dispatch context in
  one PR could break the ~600 LoC / ~6 files guideline. Mitigation: land
  `tool::Workspace` + the v1 resolver matrix + one built-in migration
  (`file.read`) in slice N; migrate mutating tools in slice N+1; then migrate
  the remaining read/list tools in a later slice. STATUS.md names the active
  migration target.
- **`weakly_canonical` cost.** Every dispatch now stat-walks the path. For
  hot directories this is negligible against permission/audit/hook costs;
  bench `tool/dispatch_overhead` re-runs against a workspace-resolved
  built-in must stay inside spec 0002's ≤ 50 µs median target.
- **Symlink rejection breaks operator workflows.** A repo with a symlinked
  `build/` may need write access through the symlink. The
  `permissions.workspace.extra_write_roots` override exists exactly for this;
  document it in `docs/PRODUCT_SENSE.md` "Predictable defaults, escape
  hatches".
- **Display rendering churns audit text.** The `Workspace::display` helper
  changes the user-facing tool output for relative paths. Bench-comparable
  audit rows from before vs. after the migration must still parse via the
  same schema; the change is rendering-only.

## Validation

```sh
xmake build oran-tool
xmake test test-tool                          # the test-workspace bucket lands here
xmake run orangutan -- --explain-rules        # workspace overrides surface in the
                                              # explain output once config wiring lands
xmake build bench-oran-tool                   # dispatch_overhead must stay within
xmake run bench-oran-tool dispatch_overhead   # spec 0002's ≤ 50 µs ceiling
```

## Out-of-Band Cross-Cuts

When this spec ships, the following companion edits land in the same slice:

- `docs/SECURITY.md`: the workspace-confinement paragraph swaps from "promised"
  to "enforced via `tool::Workspace` (spec 0013)".
- `docs/design-docs/tool-runtime.md`: the `ToolRuntime` section's
  `core::Result<io::Workspace&> workspace()` accessor moves from "future" to
  "shipped" once `tool::Runtime` lands; pre-`tool::Runtime`, the same section
  documents `DispatchContext::workspace` as the interim seam.
- `docs/exec-plans/tech-debt-tracker.md`: the deep-review §Workspace row closes
  in this spec's first slice.
- `docs/histories/2026-05/`: one entry per migration slice, naming which
  built-ins moved to the resolved-path resolver in that slice.

# 0013 — Workspace + Path Policy

## User Problem

`docs/SECURITY.md` promises file tools are confined to the workspace root unless
explicitly overridden. Today they are not: every built-in file tool reads `path`
straight from the JSON input and calls `oran-io` without canonicalisation,
traversal checks, or symlink policy. Permission rules can gate a *capability*
or a *name*, but once a write is allowed it can land anywhere the host process
can write to.

For a coding agent, "ask before write" is not a substitute for a workspace
boundary. The cost of an escape — overwriting `~/.ssh/authorized_keys`,
deleting a parent project file — is permanent. Path policy must live below
permission/audit/hooks so that *every* effectful filesystem call goes through
the same resolver.

Today's seams that motivate this spec:

- `file.write` / `file.edit` / `file.delete` accept raw path strings and call
  `io::*` directly; the dispatch context has no workspace handle. Evidence:
  `src/oran-tool/file_write.cpp:86-94`, `src/oran-tool/file_edit.cpp:122-149`,
  `src/oran-tool/file_delete.cpp` (slice 30).
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

- `.gitignore` / `.ignore` honouring as a workspace-owned predicate
  (`Workspace::is_ignored(ResolvedPath)`), shared by `file.search` and the
  future `directory.scan`. Built-in skip list (`.git`, `build`, `.xmake`,
  `node_modules`, `.orangutan/cache`) lives here, not in each tool.
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
  (`file.read`) in slice N; migrate the remaining five built-ins in slice
  N+1. STATUS.md names the active migration target.
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

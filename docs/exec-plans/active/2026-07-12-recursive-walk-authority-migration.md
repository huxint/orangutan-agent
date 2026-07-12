# Recursive Walk Authority Migration

Sub-plan of [`2026-07-11-runtime-foundations-refactor.md`](2026-07-11-runtime-foundations-refactor.md)
milestone 2 ("Filesystem authority"). Closes the last consumers still on
pathname authority so the dual authority/lifetime model can be removed.

## Goal

Move recursive `FileSearch` and recursive `DirectoryList` off
`std::filesystem::recursive_directory_iterator` + workspace-canonicalised path
strings and onto dirfd-anchored descent beneath the pinned root
`io::DirectoryAuthority`. Once both recursive consumers are anchored, delete the
now-dead string-authority resolution path in `tool::Workspace` so a single
capability model governs every filesystem built-in. End state: no filesystem
built-in resolves an untrusted path string across an await, and one path is
resolved once per call rather than up to three times.

## Scope

- In scope:
  - one synchronous dirfd-anchored tree-walk primitive in `oran-io`
    (policy-free: it yields pinned child authorities + entries to a caller sink);
  - migrate recursive `FileSearch` scan onto it (anchored file reads for matches);
  - migrate recursive `DirectoryList` enumeration onto it;
  - retire the dead string-canonicalisation branch in
    `resolve_read_through_authority` and collapse the triple resolution
    (scheduler lock key, registry pre-resolve, handler) toward one resolve;
  - update spec 0013, `io-runtime.md`, the parent plan's milestone-2 box, the
    tech-debt tracker row, and the ROADMAP workspace frontier in the owning slices.
- Out of scope:
  - new tool surfaces, schema changes, or per-call override semantics changes;
  - the future `tool::Runtime::workspace()` capability accessor (separate slice);
  - non-recursive built-ins (already anchored) and mutation tools (already anchored);
  - cross-platform (non-Linux) walk backends beyond the existing unsupported-error posture.

## Context

- Relevant docs: `docs/product-specs/0013-workspace-and-path-policy.md`,
  `docs/design-docs/io-runtime.md` (Future Slices / anchored enumeration),
  `docs/design-docs/tool-runtime.md` (Workspace Handle), parent refactor plan,
  tech-debt `review/deep-2026-07-11` (workspace dirfd bullet).
- Relevant code paths: `src/oran-io/anchored_directory.cpp`,
  `src/oran-io/directory_authority.cpp`, `include/oran/io/directory_authority.hpp`,
  `include/oran/io/file.hpp`, `src/oran-tool/file_search.cpp`,
  `src/oran-tool/directory_list.cpp`, `src/oran-tool/workspace.cpp`,
  `src/oran-agent/scheduler.cpp` (derive_lock_key).
- Constraints: C++26/GCC 16.1, asio awaitables, `core::Result`, no new thread
  pool, no exception boundary, no weakened confinement. The walk primitive stays
  policy-free (`io-runtime.md`); ignore/dotfile decisions remain in
  `tool::WorkspaceWalkFilter`.
- Compile-budget impact: `oran-io` gains one `.cpp` (Linux syscalls, no new public
  template); tool TUs shrink as `<filesystem>` recursion leaves. Expect neutral-to-down.

## Risks

- Risk: a half-migrated tree walk mixes anchored descent with a `..`/symlink
  string fallback and reintroduces an escape. Mitigation: the primitive only ever
  descends through `open_directory` (openat2 `RESOLVE_BENEATH`, `O_NOFOLLOW`); a
  symlink child is classified and skipped, never opened as a directory. No pathname
  is reopened mid-walk.
- Risk: subtle semantic drift (truncation reasons, sort order, ignore-rule scope,
  display labels) breaks existing `test-tool` contracts silently. Mitigation:
  keep `WorkspaceWalkFilter` and all cap/sort/label logic in the tool layer;
  migrate the *source of entries* only; land B and C as separate green slices.
- Risk: deleting the string-authority path breaks the per-call outside-workspace
  override (which resolves a one-off authority from a parent path). Mitigation:
  Slice D touches only the *in-workspace* dual-run; the outside override keeps its
  own `build_per_call_outside_resolved` authority path.

## Milestones

1. **Slice A** — `oran-io` anchored recursive walk primitive + `test-io`. Additive; no consumer.
2. **Slice B** — recursive `FileSearch` onto the primitive; `test-tool` green.
3. **Slice C** — recursive `DirectoryList` onto the primitive; `test-tool` green.
4. **Slice D** — retire dead string-authority resolution + collapse duplicate
   resolves; docs sync; full `xmake test` green.

## Validation

- Commands: `xmake build oran-io && xmake build test-io && xmake run test-io`;
  `xmake build test-tool && xmake run test-tool`; full `xmake -j$(nproc)` +
  `xmake test`; debug `--sanitizers=y` for the walk TU.
- Manual checks: workspace-escape attempts through a mid-tree symlinked directory;
  `..` in a nested ignore file; deep tree entry-cap; SIGINT mid-walk.
- Observability checks: audit `path_resolution` metadata unchanged for recursive
  calls; no new error kinds.
- Bench comparison: `bench-oran-tool` dispatch overhead stays within spec 0002's
  ≤ 50 µs median; recursive walk wall-clock vs. the `recursive_directory_iterator`
  baseline recorded on a repo-scale tree.

## Progress Log

- [x] Slice A: primitive + test-io + io-runtime.md.
- [x] Slice B: FileSearch migrated; spec 0013 status updated.
- [x] Slice C: DirectoryList migrated; spec 0013 status updated.
- [x] Slice D: DirectoryList's dead workspace re-resolve removed; scheduler
      lock keys collapsed onto the syscall-free `Workspace::lock_key`;
      ignore-file reads anchored beneath their pinned scopes; the read
      resolver's pathname pass retained by design (see decision log);
      parent milestone-2 box, tracker row, and ROADMAP frontier updated;
      full `xmake test` green.

## Decision Log

- 2026-07-12: recursion is built from the *existing* authority primitives
  (`open_directory` descent) plus one synchronous walk driver, not a new public
  `oran-io` template — keeps the compile budget flat and the policy boundary intact.
- 2026-07-12: policy (ignore rules, dotfiles, caps, labels) stays in
  `tool::WorkspaceWalkFilter`/the handlers; `oran-io` only supplies pinned entries.
  Matches `io-runtime.md` "the oran-io surface stays policy-free".
- 2026-07-12 (Slice B): the anchored walk keeps feeding `WorkspaceWalkFilter`
  with reconstructed absolute paths (`base / relative_path`) so ignore-rule and
  display behavior stay byte-identical while only the entry source and file
  opens migrate. Anchored ignore-file *reads* (the filter itself still opens
  `.gitignore`/`.ignore` by pathname) move to Slice D together with the
  string-authority retirement.
- 2026-07-12 (Slice D, goal amendment): implementation falsified the "now-dead
  string-authority resolution" premise. `openat2` `RESOLVE_BENEATH` rejects an
  absolute-target symlink with `EXDEV` even when the target stays inside the
  root (verified with a direct syscall probe; the resolver test that pins
  spec 0013 AC2's "follow inside symlinks" fails on a pure anchored probe).
  The pathname pass inside `resolve_read_through_authority` is therefore the
  load-bearing symlink *normaliser* — it rewrites symlink-ful spellings to the
  symlink-free canonical relative that the pinned authority executes — and
  stays, gated by `refers_to_path` so a replaced root pathname is never
  consulted. Retiring it would require an anchored canonicaliser in `oran-io`
  (componentwise `readlinkat` resolution beneath pinned roots, with cross-root
  hops); recorded as a candidate future slice, not this plan.
- 2026-07-12 (Slice D): the scheduler no longer re-runs full workspace
  resolution to derive lock keys. `Workspace::lock_key` is a lexical,
  syscall-free join against the direction's roots, so keys are deterministic
  rather than racy resolution snapshots, and a batch call's path is resolved
  once — at the registry boundary. Accepted narrowing: a read addressed
  through an in-workspace symlink spelling no longer shares a lock row with a
  write to its canonical target (mutating resolution rejects symlink
  spellings, so writes always key on the physical spelling; atomic
  rename-based writes keep concurrent readers consistent either way).
- 2026-07-12 (Slice D): `WorkspaceWalkFilter` loads `.gitignore` / `.ignore`
  through pinned authorities — the walk root's at creation, each entry's
  parent authority afterwards (pre-order visits guarantee at most one unseen
  scope per entry). Stricter posture, allowed by the "reject more symlinks,
  never weaken confinement" rule: an ignore file symlinked outside its own
  directory is skipped where the retired pathname `ifstream` followed it;
  relative in-scope symlinks still load. The unused `Workspace::walk_filter`
  convenience accessor was deleted with the signature change.

## Linked Artifacts

- Parent plan: `2026-07-11-runtime-foundations-refactor.md`.
- Product spec: `docs/product-specs/0013-workspace-and-path-policy.md`.
- Design doc: `docs/design-docs/io-runtime.md`, `docs/design-docs/tool-runtime.md`.

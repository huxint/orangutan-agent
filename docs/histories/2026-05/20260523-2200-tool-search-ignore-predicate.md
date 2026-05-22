## [2026-05-23 22:00] | Task: built-in ignore predicate for `file.search` (closes spec 0011 v1.1 "`file.search` ignore predicate" built-in subset)

### Execution Context

- Agent: `Claude Code`
- Base model: `Opus 4.7`
- Runtime: `Claude Code CLI`
- Linked plan: none — scoped to the built-in-skip-list subset of
  spec 0011 v1.1's "`file.search` ignore predicate" item.
  Honouring `.gitignore` / `.ignore` files is a larger follow-up.

### User Query

> Deeply understand the project architecture and current
> implementation progress, continue advancing the project, two
> slices, one commit per slice; ultrathink.

### Changes Overview

- Areas: `oran-tool` (`file.search` directory walk), `tests/tool`,
  version banner, the docs describing the new wire surface.
- Key actions:
  - `src/oran-tool/file_search.cpp`: schema grows
    `respect_ignore: boolean`. `SearchOptions` adds
    `bool respect_ignore{true}`. New `kIgnoredDirectoryNames`
    constexpr `std::array<std::string_view, 5>` listing the
    skip-by-default directories: `.git`, `.xmake`, `.orangutan`,
    `build`, `node_modules`. A new helper
    `is_ignored_directory_name(path)` does an exact filename
    comparison against the array (only consulted on directories,
    so a hypothetical `build.lua` regular file under a flat layout
    is unaffected). The recursive walk loop gates a fresh branch
    immediately after the existing `is_hidden` filter:
    ```cpp
    if (opts.respect_ignore && entry.is_directory(probe_ec) &&
        is_ignored_directory_name(entry_path)) {
      it.disable_recursion_pending();
      ++it;
      continue;
    }
    ```
    The new branch fires *regardless* of `include_hidden` so an
    agent opting into hidden-file scans (e.g., `.env`) still skips
    `.git/`. Disabled by `respect_ignore=false` for forensic
    searches.
  - `include/oran/tool/builtins.hpp` docstring carries the new
    `respect_ignore` field and lists the five always-skip directory
    names.
  - `tests/tool/test_registry.cpp` adds three `[respect_ignore]`
    cases and one malformed-input row:
    - "built-in ignore predicate skips build and node_modules by
      default": writes `src/a.txt`, `build/b.txt`,
      `node_modules/pkg/c.txt`; the default-shape walk surfaces
      only `src/a.txt`.
    - "respect_ignore=false re-enables build and node_modules
      walks": same tree, the opt-out call surfaces all three.
    - "ignore predicate still skips .git even with
      include_hidden=true": writes `src/a.txt` and `.git/HEAD`;
      the `include_hidden=true` call surfaces `src/a.txt` but
      *not* `.git/HEAD` — pins the documented invariant.
    - Bad-type matrix gains `"respect_ignore":"yes"`; sink-event
      tally bumps 12 → 13.
  - `src/oran-bootstrap/bootstrap.cpp` version banner bumped to
    `2.0.0-slice48`.

### Design Intent

The skip list is deliberately tiny — five entries chosen for two
properties: (1) presence in a tree is near-universal, (2) signal
density per byte is so low that an agent practically never wants
to search them. `node_modules/` and `build/` are the two no-dot
entries the spec called out; `.git`, `.xmake`, and `.orangutan` are
dot-prefixed but listed explicitly anyway so the predicate works
identically with `include_hidden=true`. The point is not to be a
universal ignore mechanism — that's what `.gitignore` honour is for,
and it's a much bigger lift — but to be the "common case ergonomic
default" so an agent's first `file.search` over a repo doesn't drown
in node_modules noise.

Firing regardless of `include_hidden` is the load-bearing invariant.
The natural user model is: `include_hidden=true` means "I want to
see hidden *files* like `.env` or `.dockerignore`", not "I want to
walk every hidden directory ever". Splitting the two — hidden-files
opt-in vs. well-known-directory skip — is what lets the predicate
stay useful for the forensic-but-not-archeological case. The
documented invariant is pinned by the `.git`-with-`include_hidden=true`
test so a future refactor can't quietly unify the two filters.

The `respect_ignore=false` opt-out exists for the rare case where
an agent does need to walk a build directory or `node_modules`
(e.g., "find the failing test in `build/test-output/`"). Making the
opt-out a boolean rather than a more sophisticated per-directory
allow list keeps the schema minimal; the rare full-walk case is
expressible as a single flag, not a list.

Honouring `.gitignore` / `.ignore` would require per-directory file
parsing, glob matching, ancestor walking, and a stable evaluation
order. The spec calls for it but it's a multi-day lift; shipping the
built-in subset first lets the high-signal default land in one slice
while leaving the surface area extensible for the larger work.

### Files Modified

- `src/oran-tool/file_search.cpp` (schema, options, skip-list
  constant, helper, walk gate, tool description)
- `include/oran/tool/builtins.hpp` (docstring)
- `tests/tool/test_registry.cpp` (3 new `[respect_ignore]` cases
  + 1 new bad-type row; sink-event tally 12 → 13)
- `src/oran-bootstrap/bootstrap.cpp` (version banner)
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/product-specs/0011-file-view-and-caching.md`
- `docs/releases/feature-release-notes.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 48, new history pointer, refreshed
  `oran-tool` test counts (127 / 1067), and the next-intended-slice
  narrative now names the remaining v1.1 follow-ups (line-offset
  index, file-view cache, regex compile cache, singleflight,
  external-edit awareness, `.gitignore` / `.ignore` honor).
- `docs/ARCHITECTURE.md` — `file.search` inventory row gains the
  new `respect_ignore?` field and the skip-list narrative.
- `docs/QUALITY_SCORE.md` — Test framework row refreshed
  (`oran-tool` 127 / 1067).
- `docs/product-specs/0011-file-view-and-caching.md` — Status block
  gains a slice-48 entry; the built-in subset of "`file.search`
  ignore predicate" is marked complete.
- `docs/releases/feature-release-notes.md` — user-visible release
  note.

### Validation

- Commands run:
  - `xmake build oran-tool`
  - `xmake build test-tool` / `xmake run test-tool '[file_search]'`
  - `xmake test` (all 10 buckets pass)
- Tests added/changed:
  - `tests/tool/test_registry.cpp` adds 3 `[respect_ignore]`
    cases plus 1 bad-type row. `tests/tool` reports 127 / 1067
    (was 124 / 1047).
- Bench impact: not measured — `is_ignored_directory_name` is a
  five-entry `std::ranges::find` on `string_view`, invoked at most
  once per directory entry that survives the existing hidden-file
  filter. Compared with the per-entry `stat` cost the walk already
  pays, the predicate cost is in the noise. The slice-20 / 24 / 47
  bench numbers in `QUALITY_SCORE.md` still apply for the
  default-shape callsite over a tree without an ignored subtree;
  trees that previously walked `node_modules` will now run faster
  by the size of the skipped subtree.
- Compile-budget delta: not measured. No new headers; the changes
  are localised to one TU (`file_search.cpp`) and one docstring.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none. v1.1's remaining items (`.gitignore` /
  `.ignore` honor, line-offset index, file-view cache, regex
  compile cache, singleflight, external-edit awareness) are tracked
  in `docs/product-specs/0011-file-view-and-caching.md` as the next
  0011 milestone.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`

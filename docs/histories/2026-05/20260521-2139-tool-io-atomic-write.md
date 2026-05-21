## [2026-05-21 21:39] | Task: atomic-write for file.edit / file.write (deep-review P0)

### Execution Context

- Agent: Claude Code (Opus 4.7)
- Base model: claude-opus-4-7
- Runtime: interactive session driven by `orangutan-deep-review.md`
- Linked plan: none — single P0 item from `exec-plans/tech-debt-tracker.md`
  (`deep-review-2026-05-21`); see `PLANS_GUIDE.md` "When NOT To Create A Plan".

### User Query

> Deepen project understanding, take stock of progress, push two more
> commits forward. Slice 31 closed the rank-0 backlog; the deep review's
> P0 list is the next surface to attack.

### Changes Overview

- Areas: `oran-io::write_text_file` (new `WriteTextOptions::atomic` flag +
  temp-then-rename commit path), `oran-tool::file.edit` (always atomic on
  the rewrite step), `oran-tool::file.write` (atomic when
  `mode == truncate`).
- Key actions:
  1. Extend `WriteTextOptions` with an opt-in `atomic` flag (default
     `false` — existing callers see no behavior change).
  2. Implement `atomic_write_blocking`: write to a sibling
     `.<name>.orangutan.tmp.<seq>` in the target's parent directory
     (so `std::filesystem::rename` stays on a single filesystem and
     therefore atomic under POSIX rename(2)), then commit via rename
     and best-effort `remove` the temp on any error path.
  3. `WriteMode::append` and `WriteMode::fail_if_exists` reject
     `atomic = true` with `invalid_argument` — the semantics are
     incompatible with temp-then-rename, and surfacing the contract
     mismatch up-front beats silently overwriting whichever side
     wins the race.
  4. Wire `file.edit` to always set `atomic = true` on the rewrite
     hop (its read-modify-write is the original BUG-4.1.1 data-loss
     footgun called out in the deep review). Wire `file.write` to
     set `atomic = true` when `mode == truncate`, so the dominant
     "rewrite this file" call shape is now crash-safe; `append` and
     `fail_if_exists` keep their current semantics because the
     atomic helper rejects them.
  5. Three new io regression tests pin the contract: successful
     atomic write leaves no `.<name>.orangutan.tmp.*` sibling
     behind, `append` + `atomic` and `fail_if_exists` + `atomic`
     reject with `invalid_argument` before any I/O, and a commit
     failure (target replaced by a directory between the open and
     the rename) preserves the pre-existing entry AND cleans up the
     temp file so no leftover survives a failed atomic commit.

### Design Intent

The deep-review BUG-4.1.1 footgun is the single highest-impact
correctness defect on the tool surface: `file.edit` composes a read
and a write, and a failure mid-write (signal, ENOSPC, the executor
process being torn down) leaves the target truncated or partially
overwritten. The fix the review recommends — temp file + rename —
is the canonical POSIX pattern; the cost is one extra stat-rename
syscall per commit and a dotfile briefly visible in the parent
directory, which the existing dotfile-skip default in
`io::list_directory` and `file.search` already hides from agent
view.

Pushing the atomic path into `oran-io` instead of `file_edit.cpp`
keeps the io layer the single home for "how do we actually write a
file safely" — and lets `file.write` opt in for free on its
dominant call shape. The opt-in is *truncate only* because temp +
rename has no coherent semantics for `append` (we'd have to read
the existing file, concatenate, and rewrite — pretty much a
different tool) or `fail_if_exists` (the rename would not preserve
the O_EXCL guarantee). Surfacing those as `invalid_argument`
prevents a caller from believing they got an atomic write when they
didn't.

Alternatives considered:

- **Land atomic on `file.edit` only, by composing the temp + rename
  inside the tool handler instead of `oran-io`.** Rejected — the
  io layer is the policy-free home for file mutation; reaching
  around it from `file_edit.cpp` would duplicate the stream-open /
  cleanup discipline and leave `file.write` exposed to the same
  partial-write footgun for no benefit.
- **Make `atomic = true` the default for `WriteMode::truncate` at
  the `oran-io` layer.** Rejected — `oran-io` is downstream of
  tool callers we have not seen yet; defaulting in the io layer
  could quietly change the meaning of `mode = truncate` for a
  future caller that depends on the through-the-symlink write
  semantic. The opt-in at the tool layer covers the cases that
  matter without taking that bet.
- **Tackle more of the P0 surface in the same slice** (content-size
  caps on `file.write`/`file.edit`, transparent hashing on the
  registry, schema validation at `Registry::add`, cancellation
  polling inside `file.search`'s `walk_and_scan`). Rejected — each
  is independent of atomic write, lives in a different file, and
  deserves its own history + tests; bundling would push past the
  ~600 LoC / ~6 file guideline and bury the audit trail.

### Files Modified

- `include/oran/io/file.hpp` — added `bool atomic` to `WriteTextOptions`
  with a docstring linking the POSIX rename(2) rationale and the
  truncate-only restriction.
- `src/oran-io/file.cpp` — added `<atomic>` + `<format>`; new
  `atomic_temp_path`, `stream_write`, and `atomic_write_blocking`
  helpers; refactored `write_text_file_blocking` to dispatch to the
  atomic helper when requested and to share `stream_write` with the
  non-atomic path; reject `atomic` + `append` / `atomic` +
  `fail_if_exists` with `invalid_argument` before any io.
- `src/oran-tool/file_edit.cpp` — pass
  `WriteTextOptions{.mode = truncate, .atomic = true}` to the
  rewrite hop.
- `src/oran-tool/file_write.cpp` — set
  `options.atomic = (options.mode == WriteMode::truncate)` so the
  dominant overwrite shape rides the safe path while append /
  fail_if_exists keep their current semantics.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` bumped
  `slice31` → `slice32`.
- `tests/io/test_file.cpp` — three new cases (happy-path commit
  leaves no temp leftover; append+atomic and fail_if_exists+atomic
  reject with `invalid_argument`; commit-failure path preserves
  the pre-existing entry and cleans up the temp).
- `docs/design-docs/io-runtime.md` — `WriteTextOptions` block gains
  the `atomic` field; a new "Atomic Writes" section documents the
  contract; the status block records the slice-32 addition.
- `docs/design-docs/tool-runtime.md` — slice-32 status note on
  `file.edit` / `file.write` calling the atomic path.
- `docs/STATUS.md` — slice 31 → 32; `Last completed history`
  pointer; refreshed `oran-io` (13 → 16 cases / 51 → 70 assertions).
- `docs/exec-plans/tech-debt-tracker.md` — strike the atomic-write
  bullet from the `deep-review-2026-05-21` P0 list.
- `docs/releases/feature-release-notes.md` — new top row for
  slice 32.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/io-runtime.md` — public surface block + new
  "Atomic Writes" section + slice-32 status note.
- `docs/design-docs/tool-runtime.md` — slice-32 status note on the
  atomic-write upgrade for `file.edit` / `file.write`.
- `docs/STATUS.md` — slice number, history pointer, `oran-io`
  test/assertion counts.
- `docs/exec-plans/tech-debt-tracker.md` — atomic-write item
  removed from the deep-review P0 list.
- `docs/releases/feature-release-notes.md` — slice-32 entry.
- Earlier histories (e.g. the slice-31 row) are intentionally not
  changed — they chronicle past state, not current behavior.

### Validation

- Commands run:
  - `xmake f -m release`
  - `xmake build oran-io oran-tool test-io test-tool` — all pass.
  - `xmake test` — 10 / 10 test targets pass.
  - `xmake build orangutan && xmake run orangutan` — prints
    `orangutan v2.0.0-slice32`.
- Tests added/changed:
  - `tests/io/test_file.cpp` — +3 cases / +19 assertions (atomic
    happy path; mode rejection; commit-failure cleanup). `test-io`
    now reports 16 cases / 70 assertions (was 13 / 51).
  - `tests/tool/test_registry.cpp` — no change; existing
    `file.write` and `file.edit` cases re-validate the wired
    `atomic = true` path without needing edits because the tool's
    public output text is unchanged.
- Bench impact: untouched. The atomic path adds one rename(2)
  syscall per truncate-mode write — single-digit microseconds on
  the bench reference hardware. Re-bench once `oran-agent` lands
  a write-heavy workload.
- Compile-budget delta: `src/oran-io/file.cpp` gained `<atomic>`
  and `<format>` (both already pulled in transitively across most
  TUs). Will be picked up by the next `scripts/measure-tu.sh`
  run; no measurable budget impact expected.

### Follow-ups

- Issues opened: none — review file lives in-tree, future agents
  can consult it directly.
- Tech-debt entries: the `deep-review-2026-05-21` row in
  `exec-plans/tech-debt-tracker.md` loses the atomic-write bullet;
  P0 still has content-size caps on `file.write`/`file.edit`,
  transparent hashing on the registry map, schema validation at
  `Registry::add`, and cancellation polling inside `file.search`'s
  `walk_and_scan`.
- Linked release note: `docs/releases/feature-release-notes.md`
  gets the slice-32 row (`tool-io-atomic-write`).

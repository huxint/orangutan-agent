## [2026-05-20 23:45] | Task: `file.delete` built-in tool (slice 30)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, orangutan-refactor`
- Linked plan: none — single-session slice that adds the
  symmetric write-side counterpart to `file.read` / `file.write` /
  `file.edit`. Fits inside `PLANS_GUIDE.md`'s "When NOT To Create A
  Plan" envelope. This is the second of the two commits the user
  requested in the same work session as slice 29.

### User Query

> 继续推进项目代码实现. 你需要推进两个 commit 的实现.
>
> (Continue advancing project code implementation. Push two commits.)

### Changes Overview

- **New `oran-io::delete_file(executor, path)` helper.** Coroutine
  wrapper over `std::filesystem::remove` that runs through the
  shared `run_blocking` template — cancel-aware at the coroutine
  boundary (the same two-checkpoint shape `read_text_file` /
  `write_text_file` / `list_directory` already use). Refuses every
  path that is not a regular file with `invalid_argument` (covers
  directories AND symlinks, even symlinks that point at a regular
  file). Returns `not_found` when no entry exists at `path` and
  when the entry vanishes between the stat and the unlink (a
  realistic race the test suite cannot reproduce but the production
  surface should classify consistently).
- **New built-in: `file.delete`.** Sixth entry in `oran-tool`'s
  catalog, registered via `tool::register_file_delete`. Capability
  is the existing `core::Capability::delete_path` from the slice-7
  vocabulary — this is the first built-in that actually requires
  it (the rest of the file built-ins use `read_file` / `write_file`
  / `edit_file` / `list_directory`). Input shape
  `{"path": <string>}`; success returns the literal text
  `deleted <path>` so the agent loop can quote a concrete fact
  without parsing a structured payload. The "refuse non-regular
  files" rule lives in `oran-io::delete_file` rather than the
  handler so any future caller of the io helper inherits the same
  safety guarantee.
- **`tool::register_builtins` grows from 5 to 6.** Slice 30 wires
  `register_file_delete` after `register_directory_list` so the
  catalog order is `file.read` → `file.write` → `file.edit` →
  `file.search` → `directory.list` → `file.delete`. The slice-17
  test that pins catalog order updates in lockstep.
- **Slice-version bump.** `kVersion` 29 → 30. `xmake run orangutan
  --help` reports `orangutan v2.0.0-slice30`.

### Design Intent

**Why refuse symlinks instead of unlinking the link.** A naïve
`std::filesystem::remove` on a symlink would unlink the link, not
its target — usually what an operator wants for a symlink they
control, but a footgun when an LLM-driven agent encounters a
symlink it did not create. The conservative v1 policy is "refuse
anything that is not a regular file"; the future unified delete
tool that supersedes this one will need explicit symlink-handling
semantics in its input, not a separate `file.delete_symlink`
tool. The cost of the narrower v1 surface is a clearer audit
trail (every `file.delete` audit row corresponds to a regular-
file removal) and a smaller blast radius when a rule allows
`delete_path` broadly.

**Why refuse directories instead of recursive remove.** Same
argument — `std::filesystem::remove_all` would delete a subtree
without confirmation, and a permission rule that allows
`delete_path` would unintentionally allow tree destruction. The
future direction is a single unified delete tool that handles
both files and folders (with recursion expressed in the input
rather than the tool name); the v1 narrow surface keeps the
audit trail honest until that refactor lands.

**Why the not-a-regular-file refusal lives in `oran-io` rather
than the tool handler.** Every future caller of `delete_file`
(`oran-skill` cleanup, `oran-storage` temp file management,
config-mutating CLI commands) inherits the same guarantee. Putting
the check in the tool handler would force each new caller to
reimplement it; the legacy `orangutan/` audit specifically lists
"duplicated safety checks per call site" as a recurring source of
divergence (see
`docs/references/orangutan-legacy-audit.md`).

**Why `delete_file` returns `not_found` on the "vanished between
stat and unlink" race.** `std::filesystem::remove` returns
`false` (no error code) when the file did not exist at unlink
time. Surfacing that as `Error::io` would lump it with real
filesystem failures; surfacing as `Error::not_found` matches the
end state the caller observes (no file at `path` after the call).
The race is unreachable from a single-threaded test, but the
production-side classification is the same shape as the upfront
stat's `not_found` branch — one error kind per end state.

**Why no per-tool bench scenario.** `file.delete` is one
`std::filesystem::symlink_status` + one `std::filesystem::remove`
behind a coroutine boundary. The perf surface is dominated by the
kernel's `unlinkat` syscall; no implementation choice in this
slice changes that. A bench scenario would measure the `unlinkat`
performance of the test filesystem, which says nothing about the
tool's design.

### Files Modified

- `include/oran/io/file.hpp` — adds `delete_file` declaration with
  its design-intent docstring.
- `src/oran-io/file.cpp` — adds `delete_file_blocking` (the
  refuse-non-regular + remove + classify-race-as-not-found body)
  and its `run_blocking`-wrapped public entry point.
- `include/oran/tool/builtins.hpp` — adds `kFileDeleteName`
  constant and `register_file_delete` declaration; bumps the slice
  marker in the file header.
- `src/oran-tool/file_delete.cpp` — new ~75-LoC TU with the
  handler + registrar (the io helper does the heavy lifting).
- `src/oran-tool/builtins.cpp` — wires `register_file_delete` into
  the aggregate `register_builtins`.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 29 → 30.
- `tests/io/test_file.cpp` — appends five new tests (~70 LoC):
  happy delete, empty-path rejection, missing file `not_found`,
  directory refusal `invalid_argument` (the directory must survive
  the refused call), and the symlink refusal that skips gracefully
  when the test filesystem does not allow `create_symlink` (some
  WSL setups).
- `tests/tool/test_registry.cpp` — bumps the `register_builtins`
  catalog-order test and appends ~145 LoC across seven tests:
  registration surface, happy path with the `deleted <path>`
  success message, missing path, directory refusal, symlink
  refusal, input-validation matrix, and the deny-verdict
  short-circuit (which asserts the file *survives* the
  permission-denied call).

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 30, history pointer, refreshed
  `Latest Library Surfaces` for `oran-io` (13/51) and `oran-tool`
  (89/712), refreshed `Next intended slice` (slice-31 candidates
  named from the slice-28 list).
- `docs/ARCHITECTURE.md` — slice-status header documents the
  slice-30 `file.delete` addition and the new
  `oran-io::delete_file` helper; `oran-tool` row mentions the new
  built-in; `oran-io` row mentions `delete_file`.
- `docs/design-docs/tool-runtime.md` — status header documents
  the slice-30 entry.
- `docs/design-docs/io-runtime.md` — public-surface section adds
  the `delete_file` declaration and the safety rationale.
- `docs/releases/feature-release-notes.md` — new top row
  `tool-file-delete`.
- `docs/histories/2026-05/20260520-2345-oran-tool-file-delete.md`
  — this file.

### Validation

- Commands run:
  - `xmake build test-io` / `xmake build test-tool` — clean.
  - `xmake run test-io --reporter=compact` —
    "All tests passed (51 assertions in 13 test cases)" (was
    33 / 8 before; +5 cases / +18 assertions for the new
    `delete_file` helper).
  - `xmake run test-tool --reporter=compact` —
    "All tests passed (712 assertions in 89 test cases)" (was
    663 / 82 before slice 30; +7 cases / +49 assertions for the
    new `file.delete` built-in including the deny-verdict
    short-circuit case).
  - `xmake run test-core / test-async / test-storage /
    test-config / test-permission / test-hook / test-cli /
    test-bootstrap` — every other bucket stays green.
  - `make ci` — clean.
- Tests added/changed: see `Files Modified`.
- Bench impact: none (no new bench scenario; perf is dominated
  by `unlinkat`).
- Compile-budget delta: one new ~75-LoC TU in `oran-tool` and
  one new function in `src/oran-io/file.cpp`; the libraries'
  budgets (`compile_budget.json.categories.oran-tool` and
  `.oran-io`) already cover the per-TU cost. No budget edit
  required.

### Follow-ups

- Issues to file: none.
- Tech-debt entries filed: none. The "no symlink unlinking" and
  "no recursive remove" choices are intentional v1 narrowings.
  The user (in the same session this slice landed) has set the
  future direction: a single unified delete tool covering files
  AND folders, not per-kind splits. Likewise `directory.list` is
  expected to evolve into a recursive whole-project enumeration,
  not a separate `directory.remove`/per-kind tree-walking
  surface. Future slices should consolidate rather than add more
  per-kind built-ins.
- Linked release note: 2026-05-20 `tool-file-delete` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: when the unified delete
  refactor lands, the `file.delete` audit-row `tool_name` is
  enough to identify v1 calls forensically; the permission rule
  surface will likely need richer capability vocabulary at that
  point (the current `delete_path` is the v1 anchor).

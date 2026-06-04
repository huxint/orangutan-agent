## [2026-05-20 23:07] | Task: `directory.list` built-in tool (slice 29)

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code in local repository checkout`
- Linked plan: none — single-session slice that extends the slice-17/18/19/20
  file-tool catalog with a thin wrapper over the existing
  `oran-io::list_directory` helper. Fits inside `PLANS_GUIDE.md`'s "When
  NOT To Create A Plan" envelope (single history, no multi-slice
  intent). This is the first of the two commits the user requested in
  the same work session as slice 30.

### User Query

> 继续推进项目代码实现. 你需要推进两个 commit 的实现.
>
> (Continue advancing project code implementation. Push two commits.)

### Changes Overview

- **New built-in: `directory.list`.** `oran-tool` now exposes a fifth
  built-in that enumerates the immediate children of a directory.
  Capability `list_directory` (new) is the gating capability so a
  permission rule can distinguish "list a directory" from "read a
  file" — important for sandbox modes where the agent should see the
  shape of a tree without reading content. The handler parses the
  JSON input, validates the typed options, dispatches the existing
  `oran-io::list_directory` coroutine, and renders one
  `<path>:<kind>:<size_bytes or '-'>` line per entry, sorted by path
  (the order `oran-io::list_directory` already enforces). Empty
  directories render the literal text `no entries`; oversize
  directories propagate the `io: directory entry limit exceeded`
  error verbatim so the caller can raise `max_entries` and retry.
- **New capability: `core::Capability::list_directory`.** Inserted in
  declaration order after `delete_path` so the file-system block
  stays contiguous; `read_file` keeps `enum_values<Capability>()`
  `.front()` and `runtime_loader` keeps `.back()` (the
  `tests/core/test_capability.cpp` invariants the slice-7 introducer
  pinned). Wire spelling `list_directory` comes from
  `core::enum_name` reflection — no per-enum forwarding shim
  (`docs/rules/code-style.md` "Enums").
- **`tool::register_builtins` grows from 4 to 5.** Slice 29 wires
  `register_directory_list` after `register_file_search` so the
  catalog order is `file.read` → `file.write` → `file.edit` →
  `file.search` → `directory.list`. The slice-17 test that pins
  catalog order updates in lockstep.
- **Slice-version bump.** `kVersion` 28 → 29. `xmake run orangutan
  --help` reports `orangutan v2.0.0-slice29`.

### Design Intent

**Why a new capability instead of reusing `read_file`.** Listing a
directory and reading a file are different operations from the
operator's point of view. A sandboxed agent that may *see* the shape
of a workspace (so it can plan) but may *not* read content benefits
from a `read_file=deny, list_directory=allow` rule. Reusing
`read_file` would prevent that policy. The cost is one enum value
and one row in every default profile that wants the distinction;
this slice intentionally does *not* touch `oran-permission`'s
`Defaults::for_mode` baselines — operators can add the rule explicitly
in `config.permissions` today, and the default-mode update lands when
the broader read-side capability story is rebalanced.

**Why pass `max_entries` through to oran-io rather than truncating in
the tool.** `oran-io::list_directory` already errors with
`io: directory entry limit exceeded` when the directory exceeds the
cap; mirroring that behavior keeps the tool a transparent wrapper.
Truncating with a `(truncated; capped at N)` summary the way
`file.search` does would require either calling `oran-io` twice
(once at `cap+1` to detect overflow, once at `cap` to fetch
entries) or extending `oran-io` to return partial results plus a
truncation flag. The first approach pays the directory walk twice;
the second changes the helper's contract for every other caller.
Neither is worth slice 29's scope when the agent can simply raise
the cap and retry on the rare overflow path. Documented in the
schema so the LLM knows the contract.

**Why the output format is `<path>:<kind>:<size_bytes or '-'>` and
not JSON.** Every existing built-in returns plain text
(`Output{.text}`); switching to JSON would force a per-tool branch in
the agent's renderer. The colon-delimited format matches
`file.search`'s `<path>:<line>:<text>` so an LLM that has seen one
already recognises the other. `kind` first would flip the visual
order; `path` first keeps the most distinctive field anchored to the
left margin where the LLM tends to skim.

**Why no per-tool bench scenario.** `directory.list` is a thin
formatter on top of `oran-io::list_directory`, which already has a
bench bucket and a measured baseline. A `bench-tool` scenario would
measure JSON parsing + a vector iteration loop — work that does not
move with the implementation choices this slice makes. Re-adding a
scenario when the renderer grows (truncation summary, structured
output, etc.) is the right time to bench. The
`testing-and-bench.md` rule "one bench if perf is plausibly affected"
applies; perf is not plausibly affected differently from the
existing io coverage.

### Files Modified

- `include/oran/core/capability.hpp` — adds `Capability::list_directory`
  after `delete_path`.
- `include/oran/tool/builtins.hpp` — adds `kDirectoryListName` constant
  and `register_directory_list` declaration; bumps the slice marker
  in the file header.
- `src/oran-tool/directory_list.cpp` — new ~120-LoC TU implementing
  the handler + registrar.
- `src/oran-tool/builtins.cpp` — wires `register_directory_list` into
  the aggregate `register_builtins`.
- `src/oran-bootstrap/bootstrap.cpp` — `kVersion` 28 → 29.
- `tests/core/test_capability.cpp` — adds the `list_directory`
  enumerator row and bumps `STATIC_REQUIRE(all.size() == 20)`.
- `tests/tool/test_registry.cpp` — bumps the `register_builtins`
  catalog-order test and appends ~190 LoC across eight new tests:
  registration surface, happy path, empty directory, hidden-file
  toggle, oversize directory, missing directory, regular-file target
  (not a directory), and the input-validation matrix.
- `bench/core/scenarios/capability.cpp` — comment now reads "20-entry
  capability universe".

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — slice 29, history pointer, refreshed
  `Latest Library Surfaces` for `oran-core` (54/370) and `oran-tool`
  (82/663), refreshed `Next intended slice` (slice 30 candidate is
  `file.delete`).
- `docs/ARCHITECTURE.md` — slice-status header notes the slice-29
  `directory.list` addition and the new `Capability::list_directory`
  enumerator; `oran-tool` row mentions the new built-in.
- `docs/design-docs/tool-runtime.md` — status header documents the
  slice-29 entry alongside slices 17 / 18 / 19 / 20 / 21 / 22 / 25.
- `docs/releases/feature-release-notes.md` — new top row
  `tool-directory-list`.
- `docs/histories/2026-05/20260520-2307-oran-tool-directory-list.md`
  — this file.

### Validation

- Commands run:
  - `xmake build test-core test-tool` — clean.
  - `xmake run test-core --reporter=compact` —
    "All tests passed (370 assertions in 54 test cases)" (was
    366 / 54 before the new `list_directory` enum row).
  - `xmake run test-tool --reporter=compact` —
    "All tests passed (663 assertions in 82 test cases)" (was
    599 / 74 before; +8 cases / +64 assertions for the new
    built-in coverage).
  - `xmake run test-async / test-io / test-storage / test-config /
    test-permission / test-hook / test-cli / test-bootstrap` — every
    other bucket stays green.
  - `make ci` — clean.
- Tests added/changed: see `Files Modified`.
- Bench impact: none (no new bench scenario; the perf surface is
  identical to `bench/io`'s existing `list_directory` coverage).
- Compile-budget delta: one new ~120-LoC TU in `oran-tool`; the
  library's budget (`compile_budget.json.categories.oran-tool`)
  already covers the per-TU cost. No budget edit required.

### Follow-ups

- Issues to file: none.
- Tech-debt entries filed: none. The "truncate vs. raise + retry"
  trade-off is intentional and documented in the tool description;
  if a real workload surfaces overflow as a pain point, the future
  enhancement would extend `oran-io::list_directory` to return
  partial results + a truncation flag and have `directory.list`
  format the summary the way `file.search` does.
- Linked release note: 2026-05-20 `tool-directory-list` row in
  `docs/releases/feature-release-notes.md`.
- Cross-references for future agents: the slice-30 `file.delete`
  slice (lands in the same work session) shares the same
  built-in-tool pattern but adds a new `oran-io` helper instead of
  reusing an existing one.

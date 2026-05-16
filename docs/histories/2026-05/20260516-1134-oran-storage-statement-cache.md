## [2026-05-16 11:34] | Task: `oran-storage` prepared-statement cache

### Execution Context

- Agent: Claude Code (Opus 4.7, 1M context)
- Base model: claude-opus-4-7
- Runtime: Claude Code CLI, repo on Linux/WSL2, GCC 16.1.1 system compiler.
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-storage-statement-cache.md`

### User Query

> 查看当前项目进度, 继续推进项目下一阶段代码的实现.

Mid-task feedback:
> 你可以使用 ranges lib: std::ranges::find_last_if. 然后提示词框架添加一些规则：
> 尽可能去使用已有的lib库的函数，优先使用 ranges 语法和现代C++的语法.
> 优先使用一些现代语法更新的函数/类代替一些同功能的函数/类.

> 把规则也写入提示词框架里面的文件，而不仅仅只是存入你的记忆.

### Changes Overview

- Areas: `oran-storage` public API, `oran-storage` implementation, storage
  tests, storage bench, design docs, architecture map, quality score, release
  notes, project rules (`critical-rules.md` C17 extension and a new
  "Algorithms And Ranges" section in `code-style.md`), AGENTS.md conventions
  list.
- Key actions:
  - Added `StatementCache`, `CachedStatement`, and `StatementCacheOptions` to
    `oran-storage`. The cache hands out move-only RAII leases keyed by SQL
    text and `sqlite3_reset` + `sqlite3_clear_bindings` the statement on
    release before returning it to the LRU list.
  - LRU eviction picks the back-of-list non-leased entry via
    `std::ranges::find_last_if`. When every entry is leased on a miss the new
    statement becomes a *transient* lease (not inserted into the cache) so the
    cache size never exceeds `capacity`.
  - `clear()` purges every unleased entry, marks leased entries `orphaned`
    (they finalize on release rather than re-enter the cache), and resets the
    hit / miss / eviction counters.
  - Re-exported the new header from `include/oran/storage.hpp`.
  - Added `tests/storage/test_statement_cache.cpp` covering option validation,
    miss/hit/eviction counters, LRU policy, transient overflow, lease
    idempotent release, double-acquire rejection, clear semantics, and the
    cache-gone-while-lease-out case.
  - Added `bench/storage/scenarios/statement_cache.cpp` registering a fresh-
    prepare vs. cached-prepare insert comparison and wired it into
    `bench/storage/main.cpp`.
  - Extended `docs/rules/critical-rules.md` C17 and added a new "Algorithms
    And Ranges" section in `docs/rules/code-style.md` codifying the
    user's preference for `std::ranges::*` and existing lib helpers over
    hand-rolled loops; surfaced the rule pointer in `AGENTS.md` "Conventions
    At A Glance".

### Design Intent

This slice closes the "prepared-statement cache" row on the storage area in
`docs/QUALITY_SCORE.md` and the matching row in
`docs/design-docs/storage-runtime.md#Future Slices`. The cache is a standalone
helper rather than a `Connection` member so the existing `Connection` /
`Statement` surface stays untouched and the pool integration (per-slot caches)
can land as a follow-up without re-shaping the storage core.

The lease-driven RAII contract mirrors the just-landed `Pool` slice: the
caller holds a move-only handle, the underlying statement returns to the cache
on destruction, and `release()` is idempotent. Transient overflow when every
entry is leased keeps misuse-of-cache callers from having to deal with a "all
slots busy" error path; the cache stays bounded and the worst case degrades to
no-cache for that one statement.

The user asked mid-slice that I prefer `std::ranges::*` over hand-rolled
loops; I swapped the LRU-victim search from a forward loop to
`std::ranges::find_last_if`, then promoted the guidance into the repo rule
set so it survives this conversation. The new bullets live in `C17` and the
new "Algorithms And Ranges" subsection of `code-style.md`, with an `AGENTS.md`
pointer so the rule shows up on the first-read pass.

### Files Modified

- `include/oran/storage/statement_cache.hpp` (new)
- `include/oran/storage.hpp`
- `src/oran-storage/statement_cache.cpp` (new)
- `tests/storage/test_statement_cache.cpp` (new)
- `bench/storage/scenarios/statement_cache.cpp` (new)
- `bench/storage/main.cpp`
- Docs listed below.

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/design-docs/storage-runtime.md` — added Statement Cache section
  (surface, semantics, error model, compile-time cost, threading) and removed
  the cache row from Future Slices.
- `docs/ARCHITECTURE.md` — slice status line updated for 2026-05-16 with the
  cache; storage row purpose updated.
- `docs/QUALITY_SCORE.md` — storage row updated to reflect the new cache; test
  framework row updated to 37 cases / 335 assertions; bench harness row notes
  the new fresh-vs-cached prepare comparison.
- `docs/releases/feature-release-notes.md` — added 2026-05-16
  storage-statement-cache row.
- `docs/rules/critical-rules.md` — C17 extended with three new bullets
  (existing-lib preference, `std::ranges::*` preference, newer-of-two-
  equivalents preference).
- `docs/rules/code-style.md` — new "Algorithms And Ranges" subsection with a
  preferred / forbidden example.
- `AGENTS.md` — added "Algorithms / ranges" row to Conventions At A Glance.
- `docs/exec-plans/active/2026-05-16-oran-storage-statement-cache.md` → moved
  to `docs/exec-plans/completed/2026-05-16-oran-storage-statement-cache.md`.

### Validation

- Commands run:
  ```sh
  xmake f -m release -y
  xmake build oran-storage
  xmake build orangutan
  xmake build test-storage
  xmake build bench-storage
  xmake run test-storage
  xmake run bench-storage
  xmake test
  make ci
  scripts/check-lib-parity.sh
  git diff --check
  ```
- Tests added/changed:
  - `tests/storage/test_statement_cache.cpp`: 12 new cases (open invalid,
    open valid, default cache rejects acquire, empty SQL rejected, miss + hit
    round-trip, release resets statement, double-acquire conflict, LRU
    eviction at capacity, transient overflow when all leased, clear()
    semantics, release-after-cache-destroyed safe, idempotent release).
  - `test-storage` total grew from 25 cases / 229 assertions to 37 cases /
    335 assertions.
- Bench impact:
  - `bench/storage` adds two scenarios:
    - `storage.fresh_prepare_insert`: ~118.8 μs / 64-row batch.
    - `storage.cached_prepare_insert`: ~62.7 μs / 64-row batch.
  - Cached prepare saves ~47% per batch on an in-memory schema (one
    `Connection::prepare` amortized across the cache lifetime vs. per-row).
- Compile-budget delta: one new public header (`statement_cache.hpp`) that
  pulls only stdlib + `<oran/core/result.hpp>` + `<oran/storage/sqlite.hpp>`,
  all already on the storage public surface. The implementation TU adds
  `<list>` and `<unordered_map>`, confined to `src/oran-storage/`.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Future-slice note: pool integration (per-slot statement cache attached to
  each writer / reader lease lifetime) is captured in
  `docs/design-docs/storage-runtime.md#Future Slices` and the storage row's
  next-step in `docs/QUALITY_SCORE.md`.
- Linked release note: `docs/releases/feature-release-notes.md` (2026-05-16
  storage-statement-cache row).

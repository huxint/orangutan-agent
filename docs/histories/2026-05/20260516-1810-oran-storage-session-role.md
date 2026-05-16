## [2026-05-16 18:10] | Task: `oran-storage` `SessionRepository` `core::Role` bridge

### Execution Context

- Agent: Claude (Opus 4.7, fast mode, max effort)
- Base model: claude-opus-4-7
- Runtime: local xmake release build, GCC 16.1 baseline
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-storage-session-role.md`

### User Query

> 详细了解项目目标，查看当前项目进度, 推进项目代码的实现.

### Changes Overview

- Areas:
  - `oran-core` public API: `parse_role` helper.
  - `oran-storage` public API: typed `Role` on session repository request /
    record.
  - `oran-core` and `oran-storage` tests.
  - `oran-storage` bench wiring.
  - Architecture / storage / memory / quality / release-notes docs.
- Key actions:
  - Added `core::parse_role(std::string_view) -> std::optional<Role>` to
    inverse `to_string_view`; rejects empty / unknown / case-mismatched
    spellings; round-trips every enumerator.
  - Switched `AppendSessionMessageRequest::role` and
    `SessionMessageRecord::role` from `std::string` to `core::Role`.
  - `SessionRepository::append_message` binds the column via
    `core::to_string_view(role)`; `read_message_row` parses the column
    back into `core::Role` via `core::parse_role` and surfaces a storage
    error (kind = `storage`) if the row contains an unknown role.
  - Dropped the empty-string validation for `role` (the enum cannot be
    empty); other field validations are unchanged.
  - Added two storage tests: a round-trip of every `core::Role`
    enumerator through append/load, and a bad-role-row rejection that
    bypasses the repository with raw SQL.
  - Updated the storage bench scenario to use `core::Role::user` /
    `core::Role::assistant` instead of string literals.

### Design Intent

The conversation-types slice landed `core::Role` but the storage
repository kept a free-form `std::string` role for the message rows.
That meant every caller above storage had to hand-type the wire string
("user", "assistant", etc.) and any typo would silently round-trip.
Typing the API boundary is the cheapest possible improvement: storage
still stores the column as text (the SQL schema is unchanged), but the
public API speaks the canonical enum and the read path errors when a
row's text does not match a known role.

Timestamps (`created_at`, `updated_at`) deliberately stay as
`std::string` for now — typing them as `core::Time` would touch every
read path, the bench output, several design-doc lines, and the test
assertions. That belongs in a follow-up so this slice stays small and
reviewable (rule C14).

### Files Modified

- `include/oran/core/role.hpp`
- `src/oran-core/role.cpp`
- `include/oran/storage/session_repository.hpp`
- `src/oran-storage/session_repository.cpp`
- `tests/core/test_role.cpp`
- `tests/storage/test_session_repository.cpp`
- `bench/storage/scenarios/session_repository.cpp`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/storage-runtime.md`
- `docs/design-docs/memory-system.md`
- `docs/releases/feature-release-notes.md`
- `docs/exec-plans/active/2026-05-16-oran-storage-session-role.md`
  (moved to `completed/`)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — `oran-storage` row now records the typed
  `core::Role` boundary on `SessionRepository`.
- `docs/QUALITY_SCORE.md` — test framework row updated to 48 / 276
  (core) and 52 / 606 (storage); storage row records the typed
  `core::Role` boundary.
- `docs/design-docs/storage-runtime.md` — the `SessionRepository`
  status note and the public-surface code block now reflect
  `core::Role` requests / records, plus the unknown-role-text rejection
  contract.
- `docs/design-docs/memory-system.md` — storage-foundation status note
  records that `role` is typed at the storage API boundary while
  `content_json` / `metadata_json` stay opaque.
- `docs/releases/feature-release-notes.md` — added the
  `storage-session-role` row with the API change, behavior change, and
  the new bench numbers.

### Validation

- Commands run:
  ```sh
  xmake build test-core && xmake run test-core
  xmake build test-storage && xmake run test-storage
  xmake build bench-storage && xmake run bench-storage
  xmake build orangutan
  xmake test
  git diff --check
  make ci
  ```
- Tests added/changed:
  - `tests/core/test_role.cpp` adds round-trip + unknown-spelling cases
    for `parse_role` (total 48 cases / 276 assertions, was 46 / 266).
  - `tests/storage/test_session_repository.cpp` adds enumerator
    round-trip and bad-role-row rejection cases (total 52 cases / 606
    assertions, was 50 / 582).
- Bench impact:
  - `bench/storage/scenarios/session_repository.cpp` keeps the same
    A/B; on this run raw-pool ~796 µs vs. repository ~967 µs per
    64-message batch.
- Compile-budget delta:
  - One small new include in the storage public header
    (`<oran/core/role.hpp>`, stdlib-only); already in PCH. Net per-TU
    cost change is negligible.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none new. Typing `created_at` / `updated_at` as
  `core::Time` is a candidate future slice.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`

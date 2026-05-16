# `oran-storage` — Session Repository `core::Role` Bridge Slice

## Goal

Promote `SessionRepository`'s wire-level `role` strings to the typed
`core::Role` enum so that callers above storage stop hand-typing `"user"` /
`"assistant"` / `"system"` / `"tool"` and instead reuse the canonical
core type. The repository still stores the role as the row's text column
— the change is purely at the API boundary.

The conversation-types slice
(`docs/exec-plans/completed/2026-05-16-oran-core-message.md`) deferred this
wiring explicitly to stay within the C14 guideline; this is the small
storage-side follow-up.

## Scope

- In scope:
  - Add `core::parse_role(std::string_view) -> std::optional<Role>` to
    `oran-core` (header + impl), so any callsite that has the textual
    role can recover the typed value.
  - Switch `AppendSessionMessageRequest::role` and
    `SessionMessageRecord::role` from `std::string` to `core::Role`.
  - Repository write path uses `to_string_view(role)` to bind the
    column; read path uses `parse_role(...)` to recover the enum and
    fails with a storage error when the column contains an unknown
    role.
  - Drop the empty-string validation for `role` (the enum cannot be
    empty); keep all other field validations.
  - Update `tests/storage/test_session_repository.cpp` and
    `bench/storage/scenarios/session_repository.cpp` for the new
    signatures.
  - Update docs: `docs/ARCHITECTURE.md`, `docs/QUALITY_SCORE.md`,
    `docs/design-docs/memory-system.md`, `docs/design-docs/storage-runtime.md`
    (where they mention the storage repository), and release notes.
- Out of scope:
  - Switching `created_at` / `updated_at` columns to `core::Time` (a
    follow-up slice; touches every read path and at least 8 doc lines).
  - Switching `content_json` to typed `core::Content` (waits for the
    `oran-provider` JSON adapter to land).
  - Memory-tier `session::Store` typed wrapper.
  - Wiring `oran-config` or `oran-bootstrap` to provision a real
    `sessions.db`.

## Context

- Relevant docs:
  - `docs/design-docs/memory-system.md` — explicitly says the typed
    `core::Message` wire lives in `oran-memory::session::Store`; the
    `oran-storage` repository is the payload-oriented layer beneath.
    Typing `role` is the first step in that direction without
    introducing `oran-memory`.
  - `docs/exec-plans/completed/2026-05-16-oran-core-message.md` —
    documents the deferral.
  - `docs/rules/critical-rules.md` (C6 — public headers light; C17 —
    modern stdlib; C3 — `Result<T>` on every fallible path).
- Relevant code paths:
  - `include/oran/core/role.hpp`, `src/oran-core/role.cpp`.
  - `include/oran/storage/session_repository.hpp`,
    `src/oran-storage/session_repository.cpp`.
  - `tests/core/test_role.cpp`, `tests/storage/test_session_repository.cpp`.
  - `bench/storage/scenarios/session_repository.cpp`.
- Constraints:
  - Public storage header may add `<oran/core/role.hpp>` (stdlib-only).
  - Read path must surface a storage-kind error for unknown role text;
    do not silently downgrade to `Role::user` or similar.
  - `parse_role` is `noexcept`, returns `std::optional<Role>` so the
    helper can be called from `noexcept` contexts.
- Compile-budget impact (if any):
  - One new tiny include in the storage public header (`<oran/core/role.hpp>`
    pulls `<cstdint>`, `<format>`, `<string_view>` — already in PCH).

## Risks

- Risk: existing rows with unknown role text would suddenly fail to
  load. Mitigation: the schema is fresh in this rewrite (no production
  data); the rejection is the right shape for any future row that
  predates an enum-range expansion.
- Risk: callers that currently pass `"user"` / `"assistant"` strings
  would silently break at compile time. That's actually the point —
  the API gets more strict, and there are only a handful of callers in
  tests/bench today.

## Milestones

1. Add active plan, settle API.
2. Add `core::parse_role` (header + impl + test cases).
3. Switch storage public API to `core::Role`.
4. Update tests, bench, docs.
5. Run validation.
6. Move plan to `completed/`.

## Validation

- Commands:
  - `xmake build test-core && xmake run test-core`
  - `xmake build test-storage && xmake run test-storage`
  - `xmake build bench-storage && xmake run bench-storage`
  - `xmake test`
  - `xmake build orangutan`
  - `git diff --check`
  - `make ci`
- Manual checks:
  - Public storage header pulls in `<oran/core/role.hpp>` and nothing
    new heavy.
  - `parse_role` returns `nullopt` for unknown text.
  - Repository load_messages surfaces a storage error when a row
    contains an unknown role.
- Bench comparison (if perf-relevant):
  - No new scenario; the repository bench keeps running the same
    raw-pool vs. repository append/load A/B. The `Role` enum bind/parse
    is straight-line and not a bench candidate.

## Progress Log

- [x] Confirm scope.
- [x] Add `core::parse_role` + tests.
- [x] Switch storage public API.
- [x] Update tests and bench.
- [x] Update docs and write history entry.
- [x] Run validation and move plan to `completed/`.

## Decision Log

- 2026-05-16: keep `created_at` and the session-record timestamps as
  `std::string` for now. Rationale: typing them as `core::Time` would
  touch every read path and several doc lines (`message_count`, the
  `read_session_row` helper, the test assertions). That belongs in its
  own follow-up so this slice stays small and reviewable.

## Linked Artifacts

- Related design doc: `docs/design-docs/memory-system.md`,
  `docs/design-docs/module-boundaries.md`.
- Related product spec: `docs/product-specs/0005-memory-system.md`.
- PRs: local change.
- History entry: `docs/histories/2026-05/<timestamp>-oran-storage-session-role.md`
- Release note: `docs/releases/feature-release-notes.md`

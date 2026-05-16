# `oran-core` — Time Utility Slice

## Goal

Land the first `oran-core` utility module beyond `Error`/`Result`: a `core::Time`
value type plus strict ISO-8601 UTC format and parse helpers. The slice gives
downstream libraries (`oran-storage`, future `oran-memory`, `oran-log`) a single
canonical absolute-time type so timestamp columns and record fields no longer
plumb raw `std::string` or `std::chrono::system_clock::time_point` across
library boundaries.

## Scope

- In scope:
  - Add `include/oran/core/time.hpp` and `src/oran-core/time.cpp`.
  - `core::Time` strong wrapper around `std::chrono::system_clock::time_point`
    with defaulted three-way comparison, an `epoch()` constructor, and
    `to_system_time_point()` accessor.
  - `core::time::now_utc()` returning a `Time`.
  - `core::time::format_iso8601_utc(Time)` returning `std::string`
    (`YYYY-MM-DDTHH:MM:SS.fffZ`, fixed millisecond precision).
  - `core::time::parse_iso8601_utc(std::string_view)` returning
    `Result<Time>`, strict about the documented shape and rejecting unknown
    suffixes or fractional digits beyond millisecond precision.
  - Tests covering round-trip, ordering, epoch handling, `now_utc` monotonicity
    (non-decreasing under a tight loop), and every documented rejection path.
  - Bench scenario comparing `std::format` chrono formatting against the
    canonical `format_iso8601_utc` helper, plus a `format`-vs-`parse` cost
    comparison.
  - Update `bench/core/main.cpp` to register the new scenarios.
  - Update `docs/ARCHITECTURE.md`, `docs/QUALITY_SCORE.md`,
    `bench/core/README.md`, `docs/releases/feature-release-notes.md`, and the
    history entry.
- Out of scope:
  - `core::str` UTF-8 helpers (separate future slice).
  - `core::Message` / `core::Content` types.
  - Timezone-aware parsing or non-UTC offsets.
  - Wiring `core::Time` into existing `SessionRepository` schema strings (kept
    as opaque text in this slice; rewiring is a follow-up that touches storage
    tests and bench separately).
  - Calendar arithmetic beyond what `std::chrono` already provides.

## Context

- Relevant docs:
  - `docs/ARCHITECTURE.md` (core inventory; lists `core::time` as a
    deliverable).
  - `docs/design-docs/memory-system.md` (`Record::created_at/updated_at/
    last_read_at` consume `core::Time`).
  - `docs/design-docs/storage-runtime.md` (`SessionRecord::created_at` /
    `updated_at` are currently opaque strings; this slice ships the typed
    equivalent for future wiring).
  - `docs/rules/critical-rules.md` (C17 — prefer modern stdlib; C7 — `explicit`
    constructors).
  - `docs/rules/error-handling.md` (parse failure returns
    `core::ErrorKind::parsing`).
  - `docs/rules/testing-and-bench.md` (every bench bucket owns an A-vs-B).
- Relevant code paths:
  - `include/oran/core/`
  - `src/oran-core/`
  - `tests/core/`
  - `bench/core/`
- Constraints:
  - Pure stdlib; no new third-party packages.
  - Public header stays light: stdlib `<chrono>`, `<compare>`, `<string>`,
    `<string_view>`, plus `<oran/core/result.hpp>`.
  - Strict parser. Use `core::ErrorKind::parsing` for shape errors and
    `core::ErrorKind::invalid_argument` for empty input.
- Compile-budget impact (if any):
  - One small public header (chrono-light) and one implementation TU.
  - One test TU and one bench scenario TU added to existing buckets.
  - No change to `oran-storage` / `oran-async` / `oran-io` public headers.

## Risks

- Risk: GCC 16.1's chrono `std::format` ABI surprises change rendered output.
  Mitigation: format the integer fields explicitly (`std::format(
  "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", …)`) instead of relying on
  chrono format specifiers, and assert the exact output in tests.
- Risk: `std::chrono::system_clock` can step backwards (NTP). Mitigation:
  `now_utc()` returns a `Time`; the "monotonic" test only requires that two
  back-to-back calls compare non-decreasing within a tight bound, and it is
  retry-tolerant if the clock steps backwards on a noisy CI machine.
- Risk: Strict parser rejects shapes the wider ecosystem accepts. Mitigation:
  the docstring states the supported subset (`YYYY-MM-DDTHH:MM:SS[.fff]Z`,
  millisecond precision optional but rejected past three digits), and the test
  enumerates rejections so behavior is pinned.
- Risk: Test-only headers leak into the public surface. Mitigation: stick to
  the existing `tests/core/test_*.cpp` pattern and avoid `<iostream>`.

## Milestones

1. Add active plan and settle MVP API.
2. Implement `core::Time` and helpers; keep formatting deterministic.
3. Add focused tests covering round-trip, ordering, and rejection paths.
4. Add bench scenarios and register them in `bench/core/main.cpp`.
5. Update docs and write release note + history.
6. Run validation and move plan to `completed/`.

## Validation

- Commands:
  - `git diff --check`
  - `xmake build test-core`
  - `xmake run test-core`
  - `xmake build bench-core`
  - `xmake run bench-core`
  - `xmake test`
  - `xmake build orangutan`
  - `make ci`
- Manual checks:
  - Confirm `format_iso8601_utc(parse_iso8601_utc(s).value()) == s` for every
    well-formed input the test enumerates.
  - Confirm public header only adds chrono-light + `<string>` /
    `<string_view>` / `<oran/core/result.hpp>` to the include graph.
- Observability checks:
  - None in this slice (pure value type).
- Bench comparison (if perf-relevant):
  - `core.time_format_explicit` vs. `core.time_format_chrono_format`: the
    explicit-format helper should be at worst comparable to the chrono-format
    baseline.
  - `core.time_parse_iso8601_utc` vs. `core.time_format_iso8601_utc`: parse
    cost is expected to dominate, and the comparison documents the budget for
    future memory/storage hot paths.

## Progress Log

- [x] Confirm scope and constraints.
- [x] Implement `core::Time` public API and storage TU.
- [x] Add core tests.
- [x] Add core bench scenarios and registration.
- [x] Update docs that this slice invalidates in the same PR.
- [x] Run validation and record results.
- [x] Write history entry.
- [x] Add release note.
- [x] Move plan to `docs/exec-plans/completed/` before finish.

## Decision Log

- 2026-05-16: format with `std::format("{:04}-…")` over chrono format specifiers.
  Rationale: pin the wire format so test assertions don't drift if the standard
  library's `%T` formatter changes precision or rounding behavior, and so the
  rendered string is stable across GCC/Clang.
- 2026-05-16: support fractional digits up to milliseconds only. Rationale:
  matches the precision the rest of the system needs (SQLite timestamps,
  session message ordering); a stricter shape now keeps the parser simple and
  lets a future slice relax it deliberately.
- 2026-05-16: keep `SessionRepository`'s string timestamps unchanged in this
  slice. Rationale: rewiring storage's typed surface widens the change set past
  the C14 size guideline; this slice ships the type, follow-ups consume it.

## Linked Artifacts

- Related design doc: `docs/design-docs/memory-system.md`
- Related product spec: `docs/product-specs/0005-memory-system.md`
- PRs: local change.
- History entry: `docs/histories/2026-05/20260516-1450-oran-core-time.md`
- Release note: `docs/releases/feature-release-notes.md`

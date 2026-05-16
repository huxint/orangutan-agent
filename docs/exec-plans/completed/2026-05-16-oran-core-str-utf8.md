# `oran-core` — `core::str` UTF-8 Helpers Slice

## Goal

Land the planned `core::str` namespace with a focused first surface: UTF-8
validation, code-point counting, and a boundary-safe byte-truncation helper.
The architecture inventory lists `core::str` as a planned core type
(`docs/ARCHITECTURE.md`) because every layer above `oran-core` ends up
massaging UTF-8 at boundaries — provider request bodies, storage payloads,
log lines, CLI echo. Centralizing the primitives here keeps the rest of the
stack from re-implementing them.

## Scope

- In scope:
  - `include/oran/core/str.hpp` and `src/oran-core/str.cpp`:
    - `core::str::is_valid_utf8(std::string_view) noexcept -> bool` (RFC
      3629 well-formed: rejects overlong encodings, lone continuation
      bytes, and UTF-16 surrogate code points U+D800..U+DFFF).
    - `core::str::count_code_points(std::string_view) noexcept ->
      std::optional<std::size_t>` (returns `nullopt` when input is not
      valid UTF-8).
    - `core::str::truncate_to_code_point(std::string_view text,
      std::size_t max_bytes) noexcept -> std::string_view` — returns the
      largest prefix that ends on a code-point boundary and fits in
      `max_bytes`. Operates on byte indices and tolerates invalid bytes
      by stopping at the last valid boundary it saw.
  - Test bucket: `tests/core/test_str_utf8.cpp` covering ASCII, multi-byte
    valid input, overlong-encoding rejection, surrogate rejection,
    truncated trailing bytes, lone continuations, and boundary-safe
    truncate for ASCII, 2/3/4-byte sequences, and the empty / short cases.
  - Bench: `bench/core/scenarios/str_utf8.cpp` comparing
    `core::str::is_valid_utf8` against a `std::ranges::all_of` ASCII-only
    short-circuit on the same fixture so future callers see the cost of
    the strict RFC-3629 walk vs. an ASCII fast path.
  - Docs: `docs/ARCHITECTURE.md` inventory + slice-status,
    `docs/QUALITY_SCORE.md` test/bench rows, `bench/core/README.md`,
    `docs/releases/feature-release-notes.md`, and a history entry.
- Out of scope:
  - Unicode normalization (NFC/NFD), case folding, grapheme clustering.
  - UTF-16/UTF-32 conversions.
  - String iterators that yield `char32_t` code points (planned follow-up
    if the agent loop wants code-point-level cursors).
  - Wiring `core::str::is_valid_utf8` into existing call sites such as
    `oran-config` JSON values or `oran-storage` row payloads (deferred to
    keep this slice within the C14 guideline).

## Context

- Relevant docs:
  - `docs/ARCHITECTURE.md` (lists `core::str` as a planned `oran-core`
    surface).
  - `docs/rules/critical-rules.md` (C7 — `explicit`; C17 — modern stdlib;
    C6 — no heavy includes in public headers).
  - `AGENTS.md` (UTF-8 conversion handled at boundaries via
    `oran::core::str::*`).
- Relevant code paths:
  - `include/oran/core/`, `src/oran-core/`, `tests/core/`, `bench/core/`.
- Constraints:
  - Pure stdlib (`<string_view>`, `<optional>`, `<cstddef>`, `<cstdint>`).
  - No exceptions (`noexcept` everywhere; the helpers are pure walks).
  - Public header pulls in `<string_view>`, `<optional>`, and `<cstddef>`
    only.
- Compile-budget impact (if any):
  - One small public header + one implementation TU + one test TU + one
    bench scenario. All inside the `oran-core` ≤ 1.5 s per-TU budget.

## Risks

- Risk: an over-clever validator silently accepts overlong / surrogate
  encodings, making downstream behavior surprising. Mitigation: implement
  the RFC-3629 byte-class table verbatim, and test each forbidden shape
  (overlong null, surrogate, truncated 2/3/4-byte sequences).
- Risk: `truncate_to_code_point` returns a confusing prefix for non-UTF-8
  input. Mitigation: doc the contract clearly — the function operates on
  byte indices; callers that need a *validated* prefix should
  `is_valid_utf8` first.

## Milestones

1. Add active plan, settle API.
2. Implement validator + counter + truncator.
3. Add focused tests.
4. Add bench scenario + register.
5. Update docs/history/release notes.
6. Run validation and move plan to `completed/`.

## Validation

- Commands:
  - `xmake build test-core && xmake run test-core`
  - `xmake build bench-core && xmake run bench-core`
  - `xmake test`
  - `xmake build orangutan`
  - `git diff --check`
- Manual checks:
  - Public header pulls in stdlib only (`<string_view>`, `<optional>`,
    `<cstddef>`).
  - No external deps anywhere in the new files.
- Observability checks:
  - None (pure helpers).
- Bench comparison (if perf-relevant):
  - `core.str_is_valid_utf8_mixed` vs. `core.str_ranges_all_of_ascii_only`
    — documents the cost of the strict RFC-3629 walk over a known mixed
    fixture vs. an ASCII fast path.

## Progress Log

- [x] Confirm scope.
- [x] Implement `core::str` helpers.
- [x] Add tests.
- [x] Add bench scenario.
- [x] Update docs.
- [x] Run validation.
- [x] Write history entry.
- [x] Add release note.
- [x] Move plan to `completed/`.

## Decision Log

- 2026-05-16: implement the validator with a byte-class table rather than a
  shift-based state machine. Rationale: the table version is straight-line
  C++ and easier to audit against RFC 3629; future call sites can swap in
  a SIMD validator behind the same signature without changing the public
  API.

## Linked Artifacts

- Related design doc: `docs/design-docs/module-boundaries.md`.
- Related product spec: n/a (foundation slice).
- PRs: local change.
- History entry: `docs/histories/2026-05/<timestamp>-oran-core-str-utf8.md`
- Release note: `docs/releases/feature-release-notes.md`

## [2026-05-16 17:40] | Task: `oran-core` `core::str` UTF-8 helpers slice

### Execution Context

- Agent: Claude (Opus 4.7, fast mode, max effort)
- Base model: claude-opus-4-7
- Runtime: local xmake release build, GCC 16.1 baseline
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-core-str-utf8.md`

### User Query

> 详细了解项目目标，查看当前项目进度, 推进项目代码的实现.

### Changes Overview

- Areas:
  - `oran-core` public API: UTF-8 boundary helpers.
  - `oran-core` tests + bench bucket.
  - Architecture/quality/release-notes docs.
- Key actions:
  - Added `core::str::is_valid_utf8`, `core::str::count_code_points`, and
    `core::str::truncate_to_code_point` as the first `core::str` surface.
  - The validator implements the RFC-3629 byte-class table verbatim:
    rejects overlong encodings (`0xC0/0xC1` leads, overlong 3/4-byte
    shapes), lone continuation bytes (`0x80-0xBF` lead), truncated
    multi-byte sequences, UTF-16 surrogate code points
    (U+D800..U+DFFF, encoded as `0xED 0xA0-0xBF ...`), and code points
    beyond U+10FFFF (`0xF4 0x90+` and `0xF5-0xFF`).
  - `truncate_to_code_point` walks byte-by-byte and stops at the last
    boundary that fits inside `max_bytes`, never splitting what looks
    like a multi-byte lead.
  - Test: `tests/core/test_str_utf8.cpp` covers ASCII, 2/3/4-byte valid
    input, overlong rejection, surrogate rejection, lone continuations,
    truncated tails, the U+D7FF / U+10FFFF accept-edge / reject-edge
    pairs, `count_code_points` happy path + `nullopt` cases, and
    `truncate_to_code_point` for 2/3/4-byte sequences plus the empty /
    short / 0-budget edges.
  - Bench: `bench/core/scenarios/str_utf8.cpp` registers
    `core.str_is_valid_utf8_mixed` vs.
    `core.str_ranges_all_of_ascii_only` over a 1024-byte mixed fixture
    under a new `bench-core/str_utf8` sub-bench in `bench/core/main.cpp`.

### Design Intent

`docs/ARCHITECTURE.md` and `AGENTS.md` already promise that UTF-8
conversion happens at boundaries via `oran::core::str::*`. Without
those primitives, every higher layer (config values, storage payloads,
CLI echo, future provider request bodies, log lines) would re-implement
validation. Centralizing the strict RFC-3629 walk here keeps the rest
of the stack honest.

The implementation uses a straight-line byte-class table rather than a
shift-based state machine because the table version is easier to audit
against the spec; a SIMD validator can later swap in behind the same
signature without changing the public API.

The bench result on this host — `core.str_is_valid_utf8_mixed` ~419 ns
per 1 KB walk vs. `core.str_ranges_all_of_ascii_only` ~1.8 ns
short-circuit — documents the *upper bound* of validator cost over
mixed content. Hot paths that can take an ASCII fast path first should;
everything else should reach for the strict validator.

### Files Modified

- `include/oran/core/str.hpp`
- `src/oran-core/str.cpp`
- `tests/core/test_str_utf8.cpp`
- `bench/core/main.cpp`
- `bench/core/scenarios/str_utf8.cpp`
- `bench/core/README.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/releases/feature-release-notes.md`
- `docs/exec-plans/active/2026-05-16-oran-core-str-utf8.md` (moved to
  `completed/` at the end of this slice)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — `oran-core` inventory now lists `core::str`
  UTF-8 helpers as implemented; slice-status note records the new
  surface.
- `docs/QUALITY_SCORE.md` — test framework row updated to 46 cases / 266
  assertions for `oran-core`; bench harness row now mentions the
  validator-vs-ASCII-shortcut scenario.
- `docs/releases/feature-release-notes.md` — added the `core-str-utf8`
  row with the validator behavior, design intent, and bench numbers.
- `bench/core/README.md` — added the `str_utf8` scenarios row.

### Validation

- Commands run:
  ```sh
  xmake build test-core
  xmake run test-core
  xmake build bench-core
  xmake run bench-core
  xmake test
  xmake build orangutan
  git diff --check
  make ci
  ```
- Tests added/changed:
  - `tests/core/test_str_utf8.cpp`: 9 cases covering happy paths and
    every RFC-3629 forbidden shape, plus boundary-safe truncation for
    ASCII, 2/3/4-byte runs, and the empty / 0-budget edges.
  - `tests/core` total is now 46 cases / 266 assertions (was 37 / 223).
- Bench impact:
  - `bench/core/scenarios/str_utf8.cpp` adds the `bench-core/str_utf8`
    sub-bench.
  - Local `xmake run bench-core` str_utf8 results:
    - `core.str_is_valid_utf8_mixed`: ~419 ns per 1024-byte walk.
    - `core.str_ranges_all_of_ascii_only`: ~1.8 ns (short-circuits on
      first non-ASCII byte in the fixture).
- Compile-budget delta:
  - One small public header (`<string_view>`, `<optional>`,
    `<cstddef>`). One new implementation TU. Both stay under the
    `oran-core` ≤ 1.5 s per-TU budget.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`

## [2026-05-16 20:06] | Task: `oran-core` `Capability` slice

### Execution Context

- Agent: Claude (Opus 4.7, fast mode, max effort)
- Base model: claude-opus-4-7
- Runtime: local xmake release build, GCC 16.1 baseline
- Linked plan: `docs/exec-plans/completed/2026-05-16-oran-core-capability.md`

### User Query

> 详细了解项目目标，查看当前项目进度, 推进项目代码的实现.

### Changes Overview

- Areas:
  - New `oran-core` data type: `core::Capability` enum + helpers.
  - `tests/core/` and `bench/core/` extended with the new bucket.
  - Architecture / quality / design-doc / release-notes updates.
- Key actions:
  - Added `core::Capability` (18 enumerators per
    `docs/design-docs/tool-runtime.md`, `std::uint8_t` underlying).
  - Added `to_string_view(Capability) noexcept` returning the stable
    spelling and falling back to `"unknown"` for out-of-range casts
    (parity with `Role` / `StopReason`).
  - Added `parse_capability(std::string_view) noexcept` returning
    `std::optional<Capability>`; refuses the `"unknown"` sentinel so
    `to_string_view`'s fallback cannot round-trip.
  - Added a constexpr `kAllCapabilities()` span so tests, schema
    generation, and the future `--explain-rules` CLI can iterate the
    universe without cast arithmetic.
  - Added `std::formatter<Capability>` so callers can `std::print`
    the value directly.
  - Added `tests/core/test_capability.cpp` — 6 cases / 90 assertions
    covering every enumerator's `to_string_view`, formatter output,
    out-of-range fallback, round-trip via `parse_capability`,
    refusal of unknown spellings (including the `"unknown"`
    sentinel), and uniqueness + endpoint sanity of
    `kAllCapabilities()`.
  - Added `bench/core/scenarios/capability.cpp` — registers
    `core.capability_parse_linear` (the library path) vs.
    `core.capability_parse_unordered_map` over the same 24-input
    batch; numbers below.
  - Wired the bench scenario through `bench/core/main.cpp`.

### Design Intent

`docs/design-docs/module-boundaries.md` explicitly lists
"`Capability` enums" on the `oran-core` side of the
"What Goes In `oran-core`?" table; placing the type there now lets
`oran-permission` (rule scope: `Rule::capability`) and the future
`oran-tool` (`ToolDef::requires`, granted-set) both read the type
without one library depending on the other.

The slice deliberately ships only the type — no `std::optional<
Capability>` on `Rule` yet, no `std::vector<Capability>` on `ToolDef`
yet. The next slice (already in the task list) plumbs the
permission-side optional; the tool-runtime side waits for `oran-tool`.

The set representation (bitset of `Capability`, sorted vector, flat
set, etc.) is intentionally not chosen here. 19 entries fit in a
`std::uint32_t` bitset; locking the representation at the core layer
would force the consumer to take what core picks. The
`kAllCapabilities()` span is what tests / schema generation /
explain-rules actually need.

The bench on this host:

| scenario                              | per 24-input batch | per lookup |
| ------------------------------------- | ------------------ | ---------- |
| `core.capability_parse_linear`        | ~272 ns            | ~11.3 ns   |
| `core.capability_parse_unordered_map` | ~234 ns            | ~9.7 ns    |

The hashmap is ~14% cheaper but both are sub-microsecond at this
batch size; the constexpr-table scan stays the simpler choice. The
A/B is recorded so the next caller can change data structure with a
real number in hand.

### Files Modified

- `include/oran/core/capability.hpp` (new)
- `src/oran-core/capability.cpp` (new)
- `tests/core/test_capability.cpp` (new)
- `bench/core/scenarios/capability.cpp` (new)
- `bench/core/main.cpp`
- `bench/core/README.md`
- `docs/ARCHITECTURE.md`
- `docs/QUALITY_SCORE.md`
- `docs/design-docs/tool-runtime.md`
- `docs/releases/feature-release-notes.md`
- `docs/exec-plans/active/2026-05-16-oran-core-capability.md` (new,
  moved to `completed/` at end of slice)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — slice-status note + `oran-core` row now
  list `Capability` alongside the existing `oran-core` surface.
- `docs/QUALITY_SCORE.md` — test framework + bench harness rows now
  list the new `tests/core` totals (54 cases / 366 assertions) and
  the new `core.capability_parse_*` bench A/B.
- `docs/design-docs/tool-runtime.md` — added a "Status (2026-05-16)"
  note pinning where the enum and its helpers actually live now
  (the design-doc snippet would otherwise read as future state).
- `docs/releases/feature-release-notes.md` — added the
  `core-capability` row with the new API surface and the bench
  numbers.
- `bench/core/README.md` — extended the "What this bucket
  benchmarks" line and the scenarios table.

### Validation

- Commands run:
  ```sh
  xmake build oran-core
  xmake build test-core && xmake run test-core
  xmake build bench-core && xmake run bench-core
  xmake build orangutan
  xmake test
  git diff --check
  make ci
  ```
- Tests added/changed:
  - `tests/core/test_capability.cpp`: 6 cases / 90 assertions
    covering enum text, formatter, out-of-range fallback,
    round-trip parse, refused unknown spellings, and
    `kAllCapabilities` shape.
- Bench impact:
  - `bench/core/scenarios/capability.cpp` registers
    `core.capability_parse_linear` (~272 ns/batch) vs.
    `core.capability_parse_unordered_map` (~234 ns/batch) over
    the same 24-input mix.
- Compile-budget delta:
  - One new public header (stdlib-only: `<cstdint>`, `<format>`,
    `<optional>`, `<span>`, `<string_view>`), one new
    implementation TU, one new test TU, one new bench TU. All
    inside the `oran-core` ≤ 1.5 s per-TU budget.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none — the consumer-side wiring is the next
  exec plan in the same series (`oran-permission` capability
  gating), already tracked in the active tasks.
- Linked release note:
  `docs/releases/feature-release-notes.md#2026-05`

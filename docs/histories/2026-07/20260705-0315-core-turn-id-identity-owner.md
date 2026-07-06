## [2026-07-05 03:15] | Task: core owns TurnId spelling and generation

### Execution Context

- Agent: Claude Code
- Base model: Claude Fable 5
- Runtime: local CLI
- Linked plan: none — deepening slice from the 2026-07-05 redundancy review
  (same session as slices 270-271).

### User Query

> Gain a deep understanding of the project architecture and implementation
> goals, comprehensively optimize redundancies and deficiencies, and carry out
> appropriate refactoring.

### Changes Overview

- Areas: `oran-core` (new `turn_id.cpp`), `oran-agent`, `oran-bootstrap`,
  `test-core`.
- Key actions: `core::TurnId` now owns its identity operations. Added
  `core::format_turn_id_hex` (the canonical 32-char lowercase operator
  spelling) and `core::generate_turn_id` (the UUIDv4-shaped entropy + counter
  + timestamp construction moved verbatim from `agent::Loop`). Deleted the
  byte-for-byte duplicate hex formatters in `bootstrap.cpp`
  (`format_turn_id_hex`) and `prompt_runner.cpp` (`format_session_id`), and
  both TU-local generators (`agent::generate_turn_id`, the weaker
  `bootstrap::generate_session_id`). New `test-core` cases pin the spelling
  and the version/variant bits.

### Design Intent

The 32-lowercase-hex spelling is a documented cross-boundary contract —
`--trace` input, trace-export JSON, and session-store keys all use it — yet
it was implemented three times (two identical formatter copies, plus the
`parse_turn_id_hex` consumer) with no single owner, and random id generation
existed twice with *different* strength (the loop's UUIDv4-shaped mix vs. the
prompt runner's plain byte-fill). Concentrating both at the type's home gives
locality (the spelling/shape can only change in one place) and quietly
upgrades session-id generation to the stronger construction — behavior-neutral
for consumers because session ids are opaque 32-hex keys. The header's old
"generation is owned by the future trace writer" note described a pre-slice-85
world; the ownership statement now matches reality. `parse_turn_id_hex` stays
in `bootstrap.cpp` because its error text is flag-specific (`--trace turn id
must be…`), which is presentation, not identity.

### Files Modified

- `include/oran/core/turn_id.hpp`
- `src/oran-core/turn_id.cpp` (new)
- `src/oran-agent/loop.cpp`
- `src/oran-bootstrap/prompt_runner.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/core/test_turn_id.cpp`
- `docs/histories/2026-07/20260705-0315-core-turn-id-identity-owner.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/ARCHITECTURE.md` — `oran-core` inventory row records the slice-272
  TurnId spelling/generation ownership.
- `docs/STATUS.md` — bumps to slice 272 and refreshes `test-core` counts.
- `docs/QUALITY_SCORE.md` — refreshes `oran-core` test counts.

Not user-visible (no CLI/byte/exit-code change), so no release-note row.

### Validation

- Commands run: `xmake -j$(nproc)` full build, `test-core` 73 cases / 471
  assertions (+2 cases for the new spelling/generation coverage), `test-agent`
  57 / 10786 (unchanged), `test-bootstrap` 187 / 1832 (unchanged), full
  `xmake test` 19/19, `make ci`.
- Tests added/changed: `format_turn_id_hex emits the canonical 32-char
  lowercase spelling` and `generate_turn_id returns distinct non-zero
  UUIDv4-shaped ids` in `tests/core/test_turn_id.cpp`.
- Bench impact: none — id formatting/generation is once-per-turn.
- Compile-budget delta: editing `turn_id.hpp` (a widely-included core header)
  forced a broad rebuild (~75 s for all targets); steady-state per-TU cost is
  unchanged (`<string>` + `result.hpp` were already in every consumer's
  include closure).

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: none (not user-visible).

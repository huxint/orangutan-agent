## [2026-05-16 23:00] | Task: migrate find!=end and find!=npos to contains()

### Execution Context

- Agent: `Claude Code`
- Base model: `Claude Opus 4.7`
- Runtime: `Claude Code, /home/huxint/projects/orangutan-refactor`
- Linked plan: none — mechanical replacement.

### User Query

> Use `contains` instead of the `find(...) != end()` / `find(...) != npos`
> syntax.

### Changes Overview

- Areas: `oran-config` recognized-field checks, every test bucket that
  matched substrings inside a rendered string.
- Key actions:
  - `std::ranges::find(rng, x) != rng.end()` → `std::ranges::contains(rng, x)`
    (three sites in `src/oran-config/config.cpp`).
  - `str.find(sub) != std::string::npos` → `str.contains(sub)` and the
    inverse `== npos` → `!str.contains(sub)`. ~27 sites across
    `tests/core/`, `tests/permission/`. Done via a uniform sed pass; the
    formatter ran afterwards.
  - Iterator-storing `find` sites stay (e.g. `auto it = m.find(k); if (it
    != m.end()) { use *it; }`) — `contains` discards the iterator and a
    pure membership rewrite would force a second lookup.

### Design Intent

`contains` was a deliberate C++20/23 addition exactly so callers stop
spelling membership tests as "did the iterator escape past the end."
The unprojected expression reads as the thing it actually checks, and
the iterator-pair form is reserved for the rare callsite that wants
the position. The migration here is mechanical; the rule that enforces
it for new code lands in the next commit.

### Files Modified

- `src/oran-config/config.cpp` — three `std::ranges::contains` rewrites.
- `tests/core/test_error.cpp` — three substring assertions.
- `tests/core/test_tool_def.cpp` — five substring assertions.
- `tests/permission/test_rule_set.cpp` — four substring assertions.
- `tests/permission/test_capability.cpp` — eleven substring assertions.
- `tests/permission/test_defaults.cpp` — two substring assertions.
- `tests/permission/test_materialize.cpp` — two substring assertions.

(`bench/core/scenarios/capability.cpp` already moved off `find != end()`
in the prior commit when the reflection migration touched it.)

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- The rule that codifies "use `contains` for membership tests" lands in
  the next commit alongside the wider rule update.
- This history entry — `docs/histories/2026-05/20260516-2300-contains-over-find.md`.

### Validation

- Commands run: `xmake build` (clean across all 8 libraries +
  `orangutan`); per-target build of every test bucket; `xmake run` for
  each test bucket.
- Tests added/changed: none — call expressions changed, semantics
  preserved. 1341 assertions across 177 test cases continue to pass.
- Bench impact: none.
- Compile-budget delta: none — both expressions lower to the same
  comparison shape.

### Follow-ups

- Issues to file: none.
- Tech-debt entry: none.
- Linked release note: none (pre-release).

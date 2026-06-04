# 2026-06-04 13:44 | Task: IO Atomic Durability

## Execution Context

- Agent: Codex
- Base model: GPT-5
- Runtime: codex-cli
- Linked plan: none

## User Query

> Start the next slice after first committing the current slice; read the
> project architecture and current implementation state before coding.

## Changes Overview

- Areas: `oran-io`, bootstrap slice tag, deep-review tracker.
- Key actions: added `io::WriteTextDurability`, made atomic writes optionally
  fsync the staged file and parent directory, and replaced process-local
  counter temp names with PID + random suffix names opened with exclusive
  creation.

## Design Intent

The remaining deep-review follow-up asked for the atomic-write path to offer a
durability tier without charging every write the fsync cost. The default
`rename_only` mode preserves slice-32 behavior. Callers that need stronger
local durability can opt into `fsync_file` or `fsync_file_and_parent`; those
modes are valid only with `atomic=true` because the contract is specifically
about temp-then-rename commits. Cache invalidation is tied to successful rename
so a later parent-directory fsync failure cannot leave stale in-process file
views behind.

## Files Modified

- `include/oran/io/file.hpp`
- `src/oran-io/file.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/io/test_file.cpp`

## Docs Updated In This PR

- `docs/design-docs/io-runtime.md` - documents the durability enum, fsync
  semantics, and PID+random exclusive temp names.
- `docs/ARCHITECTURE.md` - refreshes the `oran-io` inventory row.
- `docs/QUALITY_SCORE.md` - refreshes `test-io` counts and the IO row.
- `docs/STATUS.md` - bumps the snapshot to slice 153 and closes the tracker
  item from the open-debt summary.
- `docs/exec-plans/tech-debt-tracker.md` - removes the atomic durability
  follow-up from the live deep-review row.
- `docs/releases/feature-release-notes.md` - adds the slice 153 release note.

## Validation

- Commands run:
  - `xmake build test-io`
  - `xmake run test-io`
- Tests added/changed: atomic-write tests for old counter-temp non-reuse,
  `fsync_file`, `fsync_file_and_parent`, and pre-I/O rejection when durability
  is requested without `atomic=true`.
- Bench impact: no benchmark added; fsync durability is an explicit high-value
  write option and not a default hot path.
- Compile-budget delta: one existing `oran-io` TU gained POSIX fd/fsync helpers
  and `<random>` in the implementation only; no heavy public include was added.

## Follow-ups

- Issues opened: none.
- Tech-debt entries: the 2026-05-21 follow-up row now has only the CI xmake/test
  wiring item, gated on reference hardware.
- Linked release note: `docs/releases/feature-release-notes.md` entry
  `io-atomic-durability`.

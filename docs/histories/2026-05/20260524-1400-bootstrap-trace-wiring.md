## [2026-05-24 14:00] | Task: Bootstrap trace config-to-loop wiring

### Execution Context

- Agent: `Claude`
- Base model: `Opus 4.7`
- Runtime: `local repository checkout`
- Linked plan: none — focused continuation of specs 0017/0018.

### User Query

> Continue deeply understanding the project architecture and implementation
> progress, then keep advancing the project code implementation. Each slice
> ships one version with docs in sync.

### Changes Overview

- Areas: `oran-bootstrap`, spec-0018 config-to-loop wiring, docs/history/version.
- Key actions:
  - Added `RuntimeAssemblyOptions::trace_enabled` (default `true`) and a
    forward declaration for `storage::TraceRepository` to the bootstrap
    runtime-assembly header.
  - Taught `RuntimeAssembly::build` to construct a `storage::TraceRepository`
    on the shared audit `Pool` when both audit and trace are enabled, exposed
    `RuntimeAssembly::trace_repository()` and `trace_enabled()` accessors, and
    kept the repository pointer null when either toggle is off.
  - Threaded `config.trace().enabled` through `bootstrap::run` into
    `RuntimeAssemblyOptions::trace_enabled`, and extended the runtime-assembly
    startup banner with the new trace status.
  - Added three `[trace]` test cases covering the default-on smoke
    (`append_turn` round-trip through the new repository), explicit
    `trace_enabled=false`, and `audit_enabled=false` forcing trace off.
  - Bumped `kVersion` to `2.0.0-slice87`.

### Design Intent

Spec 0018 lists a small set of downstream items after slice 86 closed the
last loop-owned writer. The smallest one to take next is the operator
config path: `config::TraceConfig::enabled` has been parsed since slice 81
and `agent::Loop` has honored the equivalent input switch since slice 82,
but no caller threaded the operator's choice end-to-end. The agent loop is
not constructed inside the binary yet (still downstream), so wiring lands
naturally at `RuntimeAssembly` — the existing per-process assembly that
already owns the audit `Pool`. Sharing the `Pool` (rather than introducing
a second DB handle for trace) keeps the cost of the slice at one extra
`std::unique_ptr<storage::TraceRepository>` and matches the existing
audit/migration ownership pattern. The header forward-declaration keeps
`oran-bootstrap` consumers from gaining a transitive `<oran/storage.hpp>`
dependency. Only the `enabled` field is wired now — `store_raw_bodies` and
`retention_days` have no consumer yet, so wiring them speculatively would
add API surface without behavioral effect.

### Files Modified

- `include/oran/bootstrap/runtime_assembly.hpp`
- `src/oran-bootstrap/runtime_assembly.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/bootstrap/test_runtime_assembly.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 87, pointed at this history, refreshed
  test counts, and recorded the bootstrap trace wiring as the closed
  spec-0018 downstream item.
- `docs/ARCHITECTURE.md` — extended the `oran-bootstrap` table row plus
  the long-form narrative to describe the new `TraceRepository` ownership.
- `docs/product-specs/0018-first-loop-observability.md` — updated the
  storage status block, the operator-config status, and AC9 to record the
  bootstrap mapping.
- `docs/QUALITY_SCORE.md` — refreshed `oran-bootstrap` test counts, noted
  the trace ownership in the Bootstrap row, and updated the Config row's
  remaining-work pointer.
- `docs/releases/feature-release-notes.md` — added the slice 87 release
  note.

### Validation

- Commands run:
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan`
  - `xmake run test-agent`
- Tests added/changed:
  - `RuntimeAssembly::build defaults to a live TraceRepository when audit is enabled`
  - `RuntimeAssembly::build omits the TraceRepository when trace_enabled is false`
  - `RuntimeAssembly::build forces trace off when audit is disabled`
- Bench impact: none; the new branch is a one-time `unique_ptr` allocation
  inside `RuntimeAssembly::build` and not a per-turn hot path.
- Compile-budget delta: not measured; one `unique_ptr` member and three
  new accessors with no new public templates.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: hook publish rows, CLI `--trace`, binary handoff
  (threading the assembly-owned `TraceRepository` into the future
  `agent::Loop` consumer in the binary).
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)

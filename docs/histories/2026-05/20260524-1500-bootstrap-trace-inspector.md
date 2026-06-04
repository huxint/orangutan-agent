## [2026-05-24 15:00] | Task: Bootstrap `--trace <turn-id>` inspector

### Execution Context

- Agent: `Claude`
- Base model: `Opus 4.7`
- Runtime: `local repository checkout`
- Linked plan: none — focused continuation of spec 0018 acceptance criterion 10.

### User Query

> Continue deeply understanding the project architecture and implementation
> progress, then keep advancing the project code implementation.

### Changes Overview

- Areas: `oran-storage`, `oran-bootstrap`, spec-0018 operator surface, docs/history/version.
- Key actions:
  - Added the operator-level read `AuditRepository::list_events_for_turn(TurnId, std::size_t limit = 200)`
    that queries `audit_events WHERE parent_turn_id = ?` ordered by `id ASC` so the original
    `tool_use` order from spec 0017 multi-tool turns survives the trace/audit join (AC3). The
    query is intentionally not scoped to a `scope_key` because the spec-0018 CLI inspector is a
    runtime-level operator tool that joins one turn's cause-chain across whatever scope
    produced it.
  - Added `--trace <turn-id>` / `--trace=<turn-id>` to `oran-bootstrap`'s argument parser and
    a private `run_trace_inspect()` shaped like `run_audit_init`: parse the 32-char lowercase
    hex turn id into `core::TurnId`, ensure the workspace audit DB exists, open a single-
    reader `storage::Pool`, run the idempotent audit migration so the inspector tolerates a
    DB that has not yet seen `--audit-init`, look up the trace row through
    `TraceRepository::get_turn`, list the joined audit rows through the new repository
    method, and render both to stdout in the `--explain-rules`-style line format.
  - The inspector returns `Error::not_found` for a missing audit DB and for an unknown turn
    id, propagates SIGINT/SIGTERM through the existing `SignalScope` so the one-shot
    `io_context` drains promptly, and translates the resulting `Error::cancelled` into the
    shell-conventional `128 + signum` exit code.
  - Bumped `kVersion` to `2.0.0-slice88` and extended `--help` to mention the new flag.
  - Added `[trace]` storage tests covering dispatch order, cross-scope inclusion, the
    optional limit, and zero-id/zero-limit validation. Added `[bootstrap][trace]` tests
    covering parsing rejection (missing value, empty value, wrong length, uppercase hex,
    non-hex characters, all-zero), the missing-DB `not_found` path, the missing-turn
    `not_found` path, and the happy path against a populated audit DB fixture (both
    `--trace <hex>` and `--trace=<hex>` invocations).

### Design Intent

Spec 0018 lists three downstream items after slice 87 closed the
config-to-`RuntimeAssembly` wiring: hook publish rows, CLI `--trace`, and binary handoff.
Binary handoff is multi-slice — `agent::Loop` is not yet constructed inside the
`orangutan` binary, and the only shipped provider is `FakeProvider`, so the real
provider/adapter work tracked under the existing tech-debt rows is the actual blocker.
Hook publish rows entangle with the still-deferred blocking-veto hook semantics. AC10's
CLI inspector is the cleanest next slice: all of its inputs are already shipped
(`TraceRepository::get_turn` from slice 78, `AuditRepository` parent-turn-aware appends
from slice 79, `RuntimeAssembly::trace_repository()` from slice 87), it gives operators
their first user-visible payoff from the trace surface built across slices 78-87, and it
fits naturally next to `--audit-init` in the existing operator-command pattern.

The new audit repository method is the only fresh surface; the inspector and hex helper
are private to `oran-bootstrap` because they have exactly one caller. The query is
deliberately not bounded by `scope_key` — the existing scope discipline is a multi-tenant
runtime safeguard, but the CLI inspector is a runtime-level read that needs to surface
every row regardless of how the audit row was scoped. Rows still preserve their original
`scope_key` in the rendered output so the operator can see which scope produced the
cause-chain row.

Audit ordering uses `id ASC` instead of the existing `list_events` `id DESC` because the
inspector exists to reconstruct dispatch order, not "newest first" timeline browsing. The
default limit of 200 matches the existing 50/200-row patterns elsewhere in the runtime and
is more than the iteration cap (`LoopOptions::max_iterations`) for any realistic agent
turn.

### Files Modified

- `include/oran/storage/audit_repository.hpp`
- `src/oran-storage/audit_repository.cpp`
- `src/oran-bootstrap/bootstrap.cpp`
- `tests/storage/test_audit_repository.cpp`
- `tests/bootstrap/test_bootstrap.cpp`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumped to slice 88, pointed at this history, refreshed
  `oran-storage` and `oran-bootstrap` test counts, recorded the inspector as the
  closed AC10 item with the remaining `hook publish rows` and `binary handoff`
  items still downstream.
- `docs/ARCHITECTURE.md` — extended the `oran-storage` row to mention
  `list_events_for_turn` and the `oran-bootstrap` row to mention the new
  `--trace` operator command.
- `docs/product-specs/0018-first-loop-observability.md` — flipped AC10 to
  shipped status and updated the storage status block.
- `docs/QUALITY_SCORE.md` — refreshed `oran-storage` and `oran-bootstrap` test
  counts and noted the trace inspector under the Bootstrap row.
- `docs/releases/feature-release-notes.md` — added the slice 88 release note.

### Validation

- Commands run:
  - `xmake build test-storage`
  - `xmake run test-storage`
  - `xmake build test-bootstrap`
  - `xmake run test-bootstrap`
  - `xmake build orangutan`
  - `xmake run orangutan --help`
  - `xmake run orangutan --trace 11111111111111111111111111111111` (not_found path)
  - `xmake run orangutan --trace abcd` (invalid_argument path)
  - `xmake run test-agent` (no regression)
  - `xmake run test-permission` (no regression)
  - `xmake run test-tool` (no regression)
- Tests added/changed:
  - `AuditRepository::list_events_for_turn preserves dispatch order and ignores scope`
  - `AuditRepository::list_events_for_turn rejects malformed inputs`
  - `run --trace rejects missing or empty turn id`
  - `run --trace rejects malformed turn ids`
  - `run --trace reports not_found when the audit database is absent`
  - `run --trace reports not_found for unknown turn ids`
  - `run --trace returns 0 when the turn row and joined audit rows exist`
- Bench impact: none; the new query is a per-invocation read on a small audit row set,
  not a hot path.
- Compile-budget delta: not measured; one new SQL constant + one new repository method
  in an existing TU, no new public templates.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: hook publish rows + binary handoff remain the named downstream
  items on spec 0018. `--trace-export` (JSON Lines, spec 0018 v1.1) and the
  `bench/oran-storage/trace_turn_insert` scenario (AC12) are still untouched.
- Linked release note:
  [`docs/releases/feature-release-notes.md`](../../releases/feature-release-notes.md)

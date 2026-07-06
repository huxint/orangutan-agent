## [2026-07-05 02:10] | Task: bootstrap operator-command bridge dedup

### Execution Context

- Agent: Claude Code
- Base model: Claude Fable 5
- Runtime: local CLI
- Linked plan: none — pure-refactor slice from the 2026-07-05 redundancy
  review (same session as slice 270).

### User Query

> Gain a deep understanding of the project architecture and implementation
> goals, comprehensively optimize redundancies and deficiencies, and carry out
> appropriate refactoring.

### Changes Overview

- Areas: `oran-bootstrap` internals only; no public surface change.
- Key actions: concentrated the three copy-pasted one-shot operator-command
  bridges (`--audit-init`, `--trace`, `--trace-export` list mode) — each ~50
  lines of `asio::io_context` + `SignalScope` + single-reader `storage::Pool`
  open + audit migration + detached `co_spawn` + signal-beats-outcome
  unwrapping — into one `run_audit_db_command<T>(audit_path, body)` template.
  Collapsed the four identical interrupted-signal exit-code blocks in
  `bootstrap::run` into a single `exit_code_for_interrupted` lambda.
  Net −59 lines in `bootstrap.cpp`.

### Design Intent

The skeleton (fresh io_context, SIGINT/SIGTERM scope, 1-reader pool with a
4-slot statement cache, idempotent audit migration, detached coroutine with
`signals.release()`, signal check before outcome unwrap) is the load-bearing
part of every read-side operator command, and it existed in three diverging
copies — the next operator command would have been written by copy-paste,
re-deciding the signal-vs-outcome precedence each time. The helper makes the
per-command code just the reads (`get_turn`, `list_events_for_turn`,
`list_turns`) and pins the shared semantics in one place: a delivered signal
wins over the body outcome, and the audit migration always runs first so a
fresh DB that has not yet seen `--audit-init` still inspects cleanly.
Behavior is intentionally identical, including the `--trace` not-found
translation for a missing `trace_turns` row and `--audit-init`'s
directory-creation preamble, which stay at their call sites.

### Files Modified

- `src/oran-bootstrap/bootstrap.cpp`
- `docs/histories/2026-07/20260705-0210-bootstrap-operator-command-dedup.md`

### Docs Updated In This PR (Prime Directive — see `docs/rules/docs-in-sync.md`)

- `docs/STATUS.md` — bumps to slice 271 and points at this history.

No external doc invalidation beyond STATUS: the change is internal to
`src/oran-bootstrap/bootstrap.cpp` and does not affect any documented
contract, CLI flag, output byte, or exit code. Not user-visible, so no
release-note row.

### Validation

- Commands run: `xmake build orangutan`, `xmake build test-bootstrap`,
  `build/linux/x86_64/release/test-bootstrap` (187 cases / 1832 assertions —
  unchanged from slice 270), gated `--channel_qq=y` bucket re-verified during
  the same session (189 / 1872), `make ci`.
- Tests added/changed: none — the existing `--audit-init` / `--trace` /
  `--trace-export` operator-command coverage in `test-bootstrap` pins the
  preserved behavior.
- Bench impact: none; operator commands are not a hot path.
- Compile-budget delta: negative if anything — one template replaces three
  coroutine lambdas in the same TU.

### Follow-ups

- Issues opened: none.
- Tech-debt entries: none.
- Linked release note: none (not user-visible).

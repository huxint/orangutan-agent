# Persisted session working context

## Objective

Preserve task continuity under context pressure and after reopening a session,
without deleting transcript rows or adding dependencies.

## Design and scope

- Keep an authoritative transcript and a separate bounded provider view.
- Store a structured checkpoint and covered sequence in reserved session metadata;
  preserve unrelated metadata. Commit checkpoints with successful transcript suffixes.
- Load history forward in bounded pages from the checkpoint. Check context before
  every provider send, including tool iterations. Never split tool call/result groups.
- Use the existing provider execution and lifecycle hooks for tool-free summaries.
  Account for summary usage, validate the output and propagate cancellation.
- Add SQLite-consistent backups and explicit session identity import mappings.
  Import refuses existing destinations and preserves source data. No memory-scope
  remapping or new schema is needed for this slice.
- Expose explicit host context budgets. Use conservative byte-based estimates
  until a model tokenizer is supplied; do not introduce a tokenizer dependency.

OpenHands' condenser supplies the log/view separation and boundary-aware cuts;
LangMem supplies incremental coverage bookkeeping. The referenced Claude summary
prompt informs preservation of user corrections, constraints and pending work.

## Verification

Test bounded forward reads, checkpoint/suffix atomicity and conflicts, backup
integrity, import identity mapping, summary validation, repeated compaction,
tool grouping, cancellation, and reopened-session continuation. Run affected
targets, the complete release build/tests and `make ci`. Update owning contracts,
inspect the final diff and commit. Real-model behavioral evaluation requires
explicitly supplied credentials and is not replaced by scripted providers.

## Progress

- Repository contracts and reference implementations inspected.
- Implementation pending. This plan authorizes the necessary multi-file slice.

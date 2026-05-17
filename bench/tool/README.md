# bench-tool

`bench-tool` measures the cost of routing a tool call through `tool::Registry`.

A-vs-B comparisons:

- `registry.lookup`: catalog-only `Registry::find("noop")` (the agent loop
  uses this on every iteration to materialize the provider's tool catalog).
- `registry.dispatch_allow`: full `dispatch` walk — permission evaluator
  with a single matching `allow` rule, an in-memory `RecordingAuditSink`
  record, and a trivial in-process handler. The delta over `registry.lookup`
  is the dispatch overhead the agent loop pays per tool call.
- `file_write.dispatch_truncate` vs. `file_write.dispatch_append`: full
  `file.write` dispatch through the registry under each `WriteMode`,
  hitting a real 64-byte write to a tempfile. The two scenarios share
  every cost (permission eval + SHA-256 input hash + audit record + JSON
  parse + disk IO); the contrast measures whether the mode choice has
  any measurable effect at small payload sizes. This is the baseline a
  future "buffered vs. unbuffered" or "direct vs. cached" write
  optimization would need to beat.
- `file_edit.dispatch_unique_replace` vs.
  `file_edit.dispatch_replace_all_many`: full `file.edit` dispatch on a
  1 KiB seed file. The unique-replace case finds and rewrites one
  occurrence; the replace-all case rewrites 64 occurrences spaced every
  16 bytes. Both scenarios pay the same per-call overhead (permission
  eval + SHA-256 + audit + JSON parse + read + write); the contrast
  measures whether the per-match substitution cost is material against
  the bulk read/write at typical edit sizes. The baseline a future
  rope or in-place rewrite would need to beat.
- `file_search.single_file_one_match` vs.
  `file_search.recursive_dir_many_matches`: full `file.search` dispatch
  rooted at a single file (one match across ~14 lines) vs. rooted at a
  4-file / 14-line directory tree (5 matches scattered across subfolders).
  Both scenarios share the fixed dispatch costs (permission eval +
  SHA-256 + audit + JSON parse + executor hop + read of the matched
  bytes); the contrast measures the `recursive_directory_iterator` walk
  + per-file open/read overhead the agent loop pays when a tool call
  reaches for a tree rather than a single file. The baseline a future
  memory-mapped scan or parallel walker would need to beat.

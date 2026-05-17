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

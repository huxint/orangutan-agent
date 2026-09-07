# Memory

`oran-memory` owns typed conversation serialization, scoped long-term records and
recall formatting. It does not choose an application identity or call a provider.

## Session History

`session::Store` wraps `storage::SessionRepository`. Keys are explicit session
and agent values; messages retain typed text, thinking, tool-use and tool-result
blocks. Its API appends and reads conversations, with serialization owned by
memory and transactions owned by storage.

`load_tail` returns the newest messages in conversation order with row and encoded
byte limits. Defaults are 128 rows and 512 KiB; valid limits are 1–4096 rows and
1–16 MiB. Counting includes content and metadata bytes. Stop at the first row
that does not fit, preserving a contiguous suffix. The full history remains in
SQLite. The agent removes an incomplete leading exchange before prompt assembly.

`Store::append_all` serializes an entire transcript suffix before the repository
acquires its writer. Serialization or storage failure leaves the preceding
conversation intact; success appends every message in order. Single-message
`append` uses the same path. An empty suffix has no effect. Message encoding and
existing tables are preserved. Individual memory-tool effects commit under their
own dispatch contract and are not rolled back with a failed transcript commit.
Explicit import mapping remains tracked persistence work.

## Long-Term Records

A `RecordKey` is `(scope_key, id)`. Every read, write and removal supplies that
scope. Record kinds are user, feedback, project, reference and team. These are
stored classifications, not evidence of a running team subsystem.

`Fts5Backend` stores records in `memory.db`, maintains its FTS index transactionally
and filters shadowed records from ordinary recall. The `Backend` boundary
validates scoped queries before storage access. `recall(Backend&, RecallRequest)`
searches with the requested limit, updates the selected records' read timestamps
and returns owned hits and framing. The host owns the backend and its pool.

Rendering is a pure function of the selected records; timestamps, request IDs
and scores stay out of cached prompt text. Automatic recall uses the current
prompt as its query and runs once through `MemoryRecall` before the provider/tool
loop. It obeys the same permissions, hooks, audit and output limits as an explicit
tool call.

Retrieval values carry one lexical score. Tool-result JSON and memory-read hook
payloads retain `score`, `lexical_score` and a null `vector_score` for compatibility
with recorded results and hook consumers.

`MemoryRecall`, `MemoryRemember` and `MemoryForget` enter through tool validation,
permissions, hooks and audit. Their bindings receive the session's scope from
the host, never from model-provided JSON. Writes publish a blocking pre-write
gate; accepted writes, recalls and deletions publish advisory observations.

SQLite operations use the blocking executor. Hook publication and session-state
updates resume on the coordinating strand.

User database contents and migration history are preserved during API reduction.
Existing optional index tables, including sqlite-vec data, are left intact when
opening the lexical store. Record metadata, shadow flags and stored kinds retain
their existing encoding.
[Storage](storage-runtime.md) owns persistence mechanics;
[agent execution](agent-platform.md) owns parent/child session composition.

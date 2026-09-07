# Storage

`oran-storage` exposes expected-returning SQLite operations. It owns connections,
statements, migrations, bounded pool leases and session/audit/trace repositories.
Domain libraries own their own schemas above this boundary.

## Ownership And Execution

Connections and statements are RAII resources. Statements bind parameters;
SQL values never enter queries by string interpolation. A `Pool` owns one writer
and a finite set of readers. Leases are exclusive and keep shared pool state
alive. Connection setup configures the opened handle from explicit options.
Per-connection statement caches are bounded and returned statements reset before
reuse.

Acquiring a lease is asynchronous and cancellation-aware. SQL after acquisition
still runs on the caller's executor. Runtime callers must start storage work on
the blocking executor; a pool handle alone does not move synchronous SQLite off
the coordinating strand.

## Migrations

Versioned migrations run transactionally and report previous/current/applied
versions. Built-in audit and session SQL is embedded from files under
`src/oran-storage/migrations`; installed binaries do not discover source files
from their current directory. Callers may supply an explicit migration directory.
The long-term memory schema belongs to `oran-memory`.

Backups before schema changes and explicit ownership/scope import mappings are
required before a data-format migration is introduced. The current runtime
reduction leaves schema versions and persisted user rows intact. Complete
import/backup tooling is tracked in [live debt](../exec-plans/tech-debt-tracker.md).

The session skill table (`session_skill_activations`) and audit reporting view
(`audit_tool_call_rollups`) remain accessible through SQLite for compatibility.
Reopening a database and appending runtime records preserves saved session
metadata, skill rows, audit decisions and traces. Migration history is retained.

## Repositories

- `SessionRepository` keys sessions by session ID and agent key. Appends allocate
  monotonically increasing sequence numbers; reads preserve conversation order.
  `append_messages` accepts one key and an owned sequence of encoded messages.
  It validates the complete input, acquires one writer lease and commits all
  inserts and session-row updates in one transaction. Any later insert or commit
  failure rolls back the suffix; an empty suffix does not create a session.
  Single-message append delegates to this transaction boundary. `get_session`
  reads one session's metadata and message count by the same composite key.
- `load_tail` reads a contiguous newest suffix bounded by row count and UTF-8
  encoded content/metadata bytes, then returns ascending sequence order. Defaults
  are 128 rows/512 KiB. The API never prunes stored messages.
- `AuditRepository` records permission/tool outcomes, enriches the matching
  decision's metadata and reads records by scope or parent turn.
  `StorageAuditSink` adapts this repository to dispatch.
- `TraceRepository` records redacted turn metadata: IDs, origin, model/route,
  prompt hashes/byte counts, usage, timing and stop/cancellation classification.
  Records can be read by turn ID or listed with session/agent filters and a
  limit. Raw provider bodies and secret values do not belong in trace rows.

Empty required keys, invalid limits and malformed rows return explicit errors.
Transactions that do not commit roll back. Session serialization finishes before
the writer transaction starts; the repository retains the existing schema and
allocates each suffix's sequence numbers while holding that transaction.

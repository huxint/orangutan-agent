# 0018 — First-Loop Observability & Trace

## User Problem

Agent behaviour is non-deterministic by construction. Two runs of the
same prompt can produce different tool call sequences, different
provider routes, different cache states, and different errors. Without
a per-turn record that ties everything together, *every bug becomes a
guessing game*: the operator sees a wrong answer and has only the
final text + a stack of audit rows + a partial log file to reconstruct
what happened.

The deep-review §First-loop observability + the agent-loop-foundation
§8 share the same conclusion: **observability must ship with the first
loop, not be retrofitted.** Retrofitting it after tools, memory, hooks,
and providers have grown is order-of-magnitude harder because every
subsystem has to be re-instrumented separately.

Today's pieces that the agent loop should *tie together*:

- `permission::AuditEvent` records every tool decision but has no
  parent identity for the *turn* that emitted it.
- `hook::Bus::publish_advisory` returns a `PublishOutcome` per sink
  but the result is not persisted.
- `provider::Response::usage` carries token counts but there is no
  per-turn aggregator.
- `prompt::CacheSection::content_hash` (spec 0016) identifies the
  cached prefix but no row records *which* prefix was sent.
- `tool::Output::usage` (spec 0014) carries cost / bytes / files but
  no per-turn rollup.

Six failure modes the existing surface cannot answer:

1. *Which* prompt prefix was cached on iteration 4 vs iteration 3?
2. *Why* did the agent loop spend 12 seconds on iteration 2? Was it
   the provider, the tools, or a blocked hook?
3. *Which* hook sink vetoed `tool_before` for `file.write` on this
   turn?
4. *Which* tool calls in this turn used the cache (spec 0011) vs.
   hit cold storage?
5. *What* was the `cause_event_id` chain from an automation firing →
   agent turn → tool call → permission deny? (The chain is named in
   `agent-platform.md` "Observability" but no row records it.)
6. *Which* prompt builder version produced this turn's preamble?
   (The `cache_version` field exists per spec 0016; nothing logs it.)

This spec defines the trace surface that answers all six. It is
deliberately small at v1 — *one row per turn*, schema-pinned, written
to `audit.db` (the existing migration store) — and grows as new
phases (memory, channel, automation) start contributing IDs.

## Scope (v1)

The MVP is the per-turn trace row + the cause-chain primitive that
makes the existing audit rows joinable. Nothing else.

- **`oran-storage::TraceRepository`** — new repository neighbour of
  the existing `SessionRepository` and `AuditRepository`. Schema:
  **Status (slice 78, 2026-05-24):** the storage foundation is shipped.
  `<oran/storage.hpp>` exports `TraceRepository`, `TraceId`,
  `AppendTraceTurnRequest`, `TraceTurnRecord`, and
  `ListTraceTurnsOptions`; `src/oran-storage/migrations/audit/0002-trace-turns-initial.sql`
  creates `trace_turns` in the existing audit DB migration stream; and
  `storage::built_in_trace_migrations()` exposes the same complete audit DB
  migration set as `storage::built_in_audit_migrations()`. The SQLite wrapper
  now supports BLOB bind/read so turn/session ids stay 16-byte BLOB values at
  the storage boundary. The repository covers append/get/list/count,
  audit-v1-to-trace-v2 upgrade, and validation; loop writes,
  `AuditEvent::parent_turn_id`, hook publish rows, trace config, and the CLI
  inspector remain downstream.
  ```sql
  CREATE TABLE trace_turns (
    turn_id           BLOB PRIMARY KEY,             -- 16-byte UUID
    parent_turn_id    BLOB NULL,                    -- nested via agent.spawn (later)
    session_id        BLOB NOT NULL,
    agent_key         TEXT NOT NULL,
    origin            TEXT NOT NULL,                -- cli, web, channel:qq, automation:cron, ...
    route_profile     TEXT NOT NULL,                -- provider profile key
    route_model       TEXT NOT NULL,                -- vendor model id
    started_at_ns     INTEGER NOT NULL,             -- core::Time.ns
    finished_at_ns    INTEGER NOT NULL,
    stop_reason       TEXT NOT NULL,                -- end_turn, tool_use, error, cancelled, ...
    iteration_count   INTEGER NOT NULL,             -- loop iterations within this turn
    prompt_prefix_hash      INTEGER NOT NULL,       -- xxh3 from RenderedPrompt
    prompt_prefix_bytes     INTEGER NOT NULL,
    active_catalog_hash     INTEGER NOT NULL,
    deferred_catalog_hash   INTEGER NOT NULL,
    cache_creation_tokens   INTEGER NOT NULL DEFAULT 0,
    cache_read_tokens       INTEGER NOT NULL DEFAULT 0,
    input_tokens            INTEGER NOT NULL DEFAULT 0,
    output_tokens           INTEGER NOT NULL DEFAULT 0,
    cost_estimate_usd       REAL    NOT NULL DEFAULT 0,
    cancellation_phase      TEXT NULL,              -- provider | tools | hooks | null
    context_json            BLOB NOT NULL DEFAULT X'7b7d',  -- extension grab-bag
    schema_version          INTEGER NOT NULL DEFAULT 1
  );
  CREATE INDEX idx_trace_session ON trace_turns(session_id, started_at_ns);
  CREATE INDEX idx_trace_agent   ON trace_turns(agent_key, started_at_ns);
  ```
  SQL ships as audit DB migration version 2 under `migrations/audit/` via the
  existing `#embed` pattern. **Status (slice 78):** shipped as
  `storage::built_in_trace_migrations()`, intentionally equivalent to the
  complete `storage::built_in_audit_migrations()` set.
- **`oran-core::TurnId`** — 16-byte UUID type (existing pattern; new
  alias if no equivalent exists). Generated at turn start; passed
  through every dispatch context, every audit event, every hook
  payload.
- **`permission::AuditEvent::parent_turn_id`** field promoted from a
  `context` map entry to a typed column on `audit_events`. The
  migration is additive (new column with a default of
  `BLOB NULL`); old rows survive intact.
- **`hook::PublishOutcome::trace_record`** — when the dispatch
  context carries a `TurnId`, the bus produces a per-publish record
  appended to the existing `audit_events` table with a
  `event_kind=hook_publish` row. The row carries the same
  `parent_turn_id`, lets cause-chain queries find every hook that
  fired during a turn.
- **`agent::Loop::TurnContext`** — the loop's per-turn carrier that
  threads `TurnId`, `parent_turn_id`, identity, route, cancellation
  slot, and stable service refs through every callsite. The
  agent-loop-foundation note's "phase 1" (Build `TurnContext`) is
  exactly this.
- **Turn-finished publisher**. `agent::Loop::run_turn` writes one
  `trace_turns` row at terminal stop reason. The write is
  *synchronous* w.r.t. the user-visible response (the loop awaits
  the insert before returning) so the row is durable before the
  agent answers. The cost is one SQLite insert per turn (≤ 30 µs
  per the existing `bench-storage` numbers).
- **Redaction policy**. The trace row never carries raw prompt
  bytes, raw tool inputs, raw memory facts, or raw provider
  responses. Only hashes, byte counts, token counts, identifiers,
  and operator-classified text (route names, stop reasons,
  cancellation phase) appear. Raw bodies live in audit's
  `input_hash` discipline (`SHA-256(input_json)`) or behind an
  explicit debug-mode flag, never in the default trace.
- **Operator config**:
  ```jsonc
  "trace": {
    "enabled": true,            // default true; identical bytes-on-the-wire when false
    "store_raw_bodies": false,  // default false; true requires explicit operator confirmation
    "retention_days": 30        // SQLite VACUUM cadence; existing 'retention_days' pattern
  }
  ```
  `enabled=false` skips the SQLite insert entirely (still emits
  audit rows; trace is the *joining* layer).
- **CLI surface**. `orangutan --trace <turn_id>` prints the row
  plus every joined audit row (`WHERE parent_turn_id = ?`) in the
  same `--explain-rules`-style table format that already exists for
  permission rules. The query is read-only; no permission changes.

## Scope (v1.1)

- **Trace export**. JSON Lines emitter for an external SIEM. One
  line per turn; trace row + joined audit rows + joined hook
  publishes. Output to stdout, file, or `oran-http` POST endpoint.
- **Provider streaming phase rollup**. The `cancellation_phase`
  field grows to record `provider_stream`,
  `provider_initial`, `provider_complete` so a cancellation during
  streaming is distinguishable from one before the first byte.
- **Tool-call rollup** — derived view on `audit_events` that pre-
  aggregates per-turn tool counts, per-tool latencies, per-tool
  failure rates. Lives as a SQL view, not a new column.
- **Cache hit/miss counters** — the `BoundedCache` stats from spec
  0012 surface as per-turn counters
  (`prompt_block_cache_hits`, `regex_cache_hits`, `file_view_cache_hits`).
  Counters live in `context_json` until a third user appears, then
  graduate to typed columns.

## Scope (v2)

- **OpenTelemetry export**. `oran-log` exporter ships traces as
  OTLP spans; the existing trace row schema becomes a one-to-one
  mapping. Until then, trace rows are SQLite-native.
- **Per-iteration row** for very-long turns (iteration count > 8).
  v1 collapses all iterations into one row; v2 splits on the
  threshold so the cancellation phase distinguishes iteration 3
  from iteration 12.
- **Cross-process trace propagation** for orchestration (spec
  0004). A `parent_turn_id` from agent A → agent B → tool dispatch
  joins across two separate `oran-agent::Loop` instances.

## Out Of Scope

- **A trace UI**. Web UI consumption lives in spec 0007. v1 ships
  the data model and the CLI inspector; the UI is downstream.
- **Sampling**. Every turn produces a trace row in v1. Sampling is
  a v2 concern once volume justifies it.
- **Trace-driven retry**. Trace is observational; retry policy
  lives in `execution::Runtime` per
  [`../design-docs/api-portability.md`](../design-docs/api-portability.md).
- **Replacing `oran-log`**. Logs explain *what happened in
  free-form text*; audit *proves what effectful action was allowed*;
  trace *joins audit + hooks + cache + provider* into one row per
  turn. Three surfaces, each load-bearing, none redundant.

## Acceptance Criteria

1. **One row per turn.** A successful single-text turn (spec 0017
   scenario #1) produces exactly one `trace_turns` row with
   `stop_reason=end_turn`, `iteration_count=1`, `cancellation_phase=
   NULL`, and the prompt prefix hash from `RenderedPrompt::prefix_hash`.
2. **Cause-chain join.** A single-tool turn (spec 0017 scenario #2)
   produces one trace row + one audit row whose `parent_turn_id`
   matches the trace row's `turn_id`. The join query in the CLI
   inspector returns both.
3. **Multi-tool fan-out.** A multi-tool turn (spec 0017 scenario
   #3, sequential dispatch in v1) produces one trace row + N audit
   rows, all sharing the same `parent_turn_id`. Sorted by
   `started_at_ns`, the audit rows preserve the original
   `tool_use` order.
4. **Cancellation phase recorded.** Cancellation during provider
   await (spec 0017 scenario #9) produces a trace row with
   `cancellation_phase='provider'`, `stop_reason='cancelled'`.
   Cancellation during tool dispatch (scenario #10) produces
   `cancellation_phase='tools'`. **Status (slice 77):** the trace row is not
   implemented yet, but `agent::Loop` now returns parent-cancelled provider /
   tool failures with `reason=parent_cancelled` and
   `cancellation_phase=provider|tools`, giving the future writer a stable
   source value.
5. **Hook publish observable.** A blocking `tool_before` veto
   (spec 0015) appends a `hook_publish` audit row with
   `parent_turn_id` matching the trace row; the row's
   `context.decision_kind='veto'` and `context.sink_id='<id>'` so
   the cause-chain shows *which sink* vetoed.
6. **Token / cost rollup.** A turn whose provider response carries
   `Usage = { input_tokens: 1500, output_tokens: 200,
   cache_read_tokens: 4096, cost_estimate: 0.012 }` writes the
   same values into `trace_turns`. Pinned by a fake-provider
   plan.
7. **Cache-version visibility.** Bumping
   `prompt::CacheSection::cache_version` (spec 0016) on a section
   changes `prompt_prefix_hash` in the next turn's row. The
   monotonic-version change is visible to an operator running
   `--trace`.
8. **Redaction default.** A turn whose tool calls carry sensitive
   inputs (a `file.write` of secret content) produces a trace
   row whose `context_json` is empty `{}` and whose audit rows
   carry `input_hash` only — no raw bytes. Pinned by a
   secret-pattern test.
9. **Trace disabled is byte-identical.** Setting
   `trace.enabled=false` produces zero `trace_turns` rows and
   leaves `audit_events` rows unchanged byte-for-byte (parent_turn_id
   is NULL when trace is off). Pinned by a side-by-side test.
10. **CLI inspector.** `orangutan --trace <turn_id>` returns
    the trace row + every joined audit row + every joined
    `hook_publish` row in deterministic order; exit code 0.
    Unknown `<turn_id>` exits non-zero with `Error::not_found`.
11. **Schema migration.** Migrating an existing `audit.db` from
    schema 1 to schema with the trace tables succeeds idempotently;
    re-running the migration is a no-op. Pinned by a migration
    test against a fixture from a pre-spec-0018 build.
12. **Insert cost.** `bench/oran-storage/trace_turn_insert` reports
    ≤ 50 µs per insert on the bench fixture, matching the existing
    `audit_event_append` numbers (~18 µs end-to-end through
    SQLite). The trace insert is one row plus the per-tool audit
    inserts the loop already pays for.

## Design Doc Cross-References

- [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)
  "Observability" — names the per-action structured log event with
  `agent_id`, `runtime_id`, `origin`, `cause_event_id`,
  `latency_ms`. This spec turns the *names* into *columns* on
  `trace_turns`.
- [`../RELIABILITY.md`](../RELIABILITY.md) "Logging" — owns the
  log surface (free-form text); this spec is the trace surface
  (joinable rows). The two surfaces share `oran-log` once that
  library lands but produce different output.
- [`../design-docs/storage-runtime.md`](../design-docs/storage-runtime.md)
  — the `TraceRepository` follows the existing `SessionRepository`
  + `AuditRepository` pattern: typed insert / query API, SQL under
  `migrations/`, async wrapper over `storage::Pool`.
- [`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)
  — `AuditEvent` gains `parent_turn_id`; the design doc edit
  lands in the same slice as v1.
- [`0014-structured-tool-output.md`](0014-structured-tool-output.md)
  — `tool::ToolUsage` fields propagate into per-turn rollups in
  v1.1.
- [`0015-blocking-hook-decisions.md`](0015-blocking-hook-decisions.md)
  — `HookDecision` audit rows join via `parent_turn_id`.
- [`0016-prompt-and-tool-catalog-cache.md`](0016-prompt-and-tool-catalog-cache.md)
  — `RenderedPrompt::prefix_hash`, `active_catalog_hash`,
  `deferred_catalog_hash` are recorded verbatim.
- [`0017-fake-provider-first-agent-loop.md`](0017-fake-provider-first-agent-loop.md)
  — the loop's `TurnContext` is the trace primitive. Every spec
  0017 acceptance criterion is testable with trace rows as
  evidence.
- [`0010-benchmark-harness.md`](0010-benchmark-harness.md) — the
  `trace_turn_insert` bench scenario lives here once authored.

## Risks

- **Schema churn.** Trace columns are tempting to add; every
  addition is a migration. Mitigation: `context_json` is the
  extension grab-bag for v1; promote a column only after a third
  consumer reads it. Same discipline as `permission::AuditEvent.
  context`.
- **Per-turn insert latency.** A synchronous SQLite insert per
  turn can dominate cheap turns. Mitigation: the
  `audit_event_append` bench already shows ~18 µs through the
  existing `storage::Pool`; the per-turn cost is one extra insert
  of the same magnitude. v1.1 can move trace to a batched
  background writer if the bench grows.
- **PII leak via `context_json`.** A future caller stashes a raw
  body in the grab-bag and bypasses redaction. Mitigation: the
  CLI inspector defaults to printing `context_json` as a hash,
  not the body; `--show-raw` requires an explicit flag and
  records its own audit row.
- **Trace disabled silently erodes observability.** Operators
  may disable trace for performance and lose joinability.
  Mitigation: the CLI inspector and `--explain-rules`-style
  surfaces refuse to run when `trace.enabled=false` and tell the
  operator to re-enable. The audit surface still works.

## Validation

```sh
xmake build oran-storage oran-agent
xmake test test-storage                       # trace_turns migration + insert + query
xmake test test-agent                         # trace rows joined to spec 0017 scenarios
xmake build bench-oran-storage
xmake run bench-oran-storage trace_turn_insert
xmake run orangutan -- --trace <turn-id>      # CLI inspector
xmake run orangutan -- --trace-export jsonl   # v1.1 surface
```

## Out-of-Band Cross-Cuts

- `docs/ARCHITECTURE.md` — `oran-storage` gains the
  `TraceRepository` line.
- `docs/design-docs/storage-runtime.md` — the `audit.db` schema
  documentation gains the `trace_turns` table + the new
  `parent_turn_id` column on `audit_events`.
- `docs/design-docs/permissions-and-hooks.md` — `AuditEvent`
  documentation gains `parent_turn_id` as a typed field; the
  `context` map's pre-spec entry retires.
- `docs/RELIABILITY.md` "Metrics And Tracing" — points at this
  spec for the per-turn schema; describes how OpenTelemetry
  export bolts on in v2.
- `docs/exec-plans/tech-debt-tracker.md` — no entry retires
  directly; the existing 2026-05-14 "Generated config schema" row
  is a near-neighbour pattern (typed extraction from C++ types)
  but is independent.
- `docs/SECURITY.md` — gains a "Redaction in observability" note
  cross-linking to the v1 redaction policy.

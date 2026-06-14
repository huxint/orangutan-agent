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
  **Status (slice 87, 2026-05-24):** the storage foundation, the first
  direct-dispatch audit join key, the first terminal-success loop writer, the
  explicit trace-disabled loop policy, and provider/tool cancellation trace
  rows are shipped. The first ordinary error rows are also shipped for
  non-cancelled provider failures and response-backed loop-boundary failures.
  `agent::Loop` now generates a non-zero turn id when an enabled trace writer
  is configured and callers leave `RunTurnInputs::turn_id` unset, and persists
  iteration-cap exits with a body-free `stop_reason=error` row before the
  existing `Error::internal` (reason=`iteration_cap`) returns. `bootstrap::run`
  now threads `config.trace().enabled` into `RuntimeAssemblyOptions::trace_enabled`
  so `RuntimeAssembly::build` constructs a `storage::TraceRepository` on the
  shared audit `Pool` whenever both audit and trace are enabled; slice 101's
  `AgentPromptRunner` consumes the assembly-exposed pointer for
  caller-supplied provider backends, while the ordinary binary handoff now
  uses the same runner through slice 112's configured-route
  `bootstrap::run` plus `HttpProviderBackend`.
  `<oran/storage.hpp>` exports `TraceRepository`, `TraceId`,
  `AppendTraceTurnRequest`, `TraceTurnRecord`, and
  `ListTraceTurnsOptions`; `src/oran-storage/migrations/audit/0002-trace-turns-initial.sql`
  creates `trace_turns` in the existing audit DB migration stream; and
  `storage::built_in_trace_migrations()` exposes the same complete audit DB
  migration set as `storage::built_in_audit_migrations()`. The SQLite wrapper
  now supports BLOB bind/read so turn/session ids stay 16-byte BLOB values at
  the storage boundary. Slice 79 adds `core::TurnId`,
  `audit_events.parent_turn_id` as audit DB migration version 3, and typed
  `parent_turn_id` fields on storage/permission audit requests and records.
  Slice 93 adds `audit_events.event_kind` as audit DB migration version 4 so
  ordinary permission decisions and `hook_publish` observability rows can share
  the same table and parent-turn cause chain without overloading metadata.
  Slice 243 adds audit DB migration version 5 with the
  `audit_tool_call_rollups` view, keeping per-turn/per-tool tool-call
  aggregates as a SQL-derived read instead of adding trace columns.
  The repository covers append/get/list/count, audit-v1-to-trace-v2/v3
  upgrade, and validation; `agent::Loop` now uses it for terminal-success rows,
  skips it when `TraceContext::enabled=false`, writes cancelled rows for
  provider/tool parent cancellation, writes ordinary provider/loop-boundary
  error rows, and writes iteration-cap error rows when
  `LoopOptions::max_iterations` is exhausted. Slice 244 refines provider
  cancellation rows to distinguish `provider_initial`, `provider_stream`, and
  `provider_complete` on the existing `cancellation_phase` field.
  `bootstrap::run` now constructs the assembly-owned `TraceRepository` from
  `config.trace().enabled`. Slice 93
  writes direct-dispatch blocking `tool_before` hook-publish rows when the
  dispatch context carries a parent turn id. Slice 101's `AgentPromptRunner`
  now threads the assembly repository into `RunTurnInputs::trace` for
  caller-supplied provider backends; slice 112 wires the same runner into
  ordinary configured-route `bootstrap::run`, so trace rows are written from
  the binary path as soon as a `default` provider route is declared. Slice 127
  adds `TraceRepository::list_provider_usage_rollups`, a read-only derived query
  that groups recorded provider usage by UTC day, agent key, route profile, and
  route model without adding new trace columns. Slice 150 adds
  `TraceRepository::purge_turns_started_before(...)` so retention can delete
  only `trace_turns` rows older than an explicit `started_at_ns` cutoff while
  leaving durable audit rows intact.
  ```sql
  CREATE TABLE trace_turns (
    turn_id           BLOB PRIMARY KEY,             -- 16-byte UUID
    parent_turn_id    BLOB NULL,                    -- nested via agent.spawn (later)
    session_id        BLOB NOT NULL,
    agent_key         TEXT NOT NULL,
    origin            TEXT NOT NULL,                -- cli, desktop, channel:qq, automation:cron, ...
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
    cancellation_phase      TEXT NULL,              -- provider_initial | provider_stream | provider_complete | tools | hooks | null
    context_json            BLOB NOT NULL DEFAULT X'7b7d',  -- extension grab-bag
    schema_version          INTEGER NOT NULL DEFAULT 1
  );
  CREATE INDEX idx_trace_session ON trace_turns(session_id, started_at_ns);
  CREATE INDEX idx_trace_agent   ON trace_turns(agent_key, started_at_ns);
  ```
  SQL ships as audit DB migration version 2 under `migrations/audit/` via the
  existing `#embed` pattern. **Status (slice 79):** shipped as
  `storage::built_in_trace_migrations()`, intentionally equivalent to the
  complete `storage::built_in_audit_migrations()` set, now including version 5
  for the `audit_tool_call_rollups` view.
- **`oran-core::TurnId`** — 16-byte UUID type (existing pattern; new
  alias if no equivalent exists). Generated at turn start; passed
  through every dispatch context, every audit event, every hook
  payload. **Status (slice 85):** `core::TurnId` is the shared 16-byte
  `std::array<std::byte, 16>` value shape; `agent::Loop` generates the first
  trace-owned ids when a trace writer is configured and the caller does not
  provide one.
- **`permission::AuditEvent::parent_turn_id`** field promoted from a
  `context` map entry to a typed column on `audit_events`. The
  migration is additive (new column with a default of
  `BLOB NULL`); old rows survive intact. **Status (slice 79):** shipped for
  direct tool-dispatch audit rows, including same-row usage metadata update
  scoping by parent turn id.
- **`hook::PublishOutcome::trace_record`** — when the dispatch
  context carries a `TurnId`, the bus produces a per-publish record
  appended to the existing `audit_events` table with a
  `event_kind=hook_publish` row. The row carries the same
  `parent_turn_id`, lets cause-chain queries find every hook that
  fired during a turn. **Status (slice 93):** direct
  `tool::Registry::dispatch` now records the v1 blocking
  `tool_before` publish outcome when `DispatchContext::parent_turn_id`
  is set and the blocking bus consulted at least one sink. The
  persisted row uses `event_kind=hook_publish`, carries the same
  parent turn id as the permission decision, and stores
  `metadata_json.event`, `sink_id`, `decision_kind`, `reason`,
  optional `elapsed_ms` / `error`, and `hook_decisions[]`.
- **`agent::Loop::TurnContext`** — the loop's per-turn carrier that
  threads `TurnId`, `parent_turn_id`, identity, route, cancellation
  slot, and stable service refs through every callsite. The
  agent-loop-foundation note's "phase 1" (Build `TurnContext`) is
  exactly this. **Status (slice 85):** the public interim surface is
  `RunTurnInputs::turn_id`; when set and trace is enabled, direct tool
  dispatches receive it as `DispatchContext::parent_turn_id`. When the turn id
  is unset but an enabled trace writer is configured, the loop generates one
  before the first prompt render and uses it for both `trace_turns.turn_id` and
  direct-dispatch audit parents. When trace is disabled, the loop forces
  `parent_turn_id = NULL` for the dispatch duration and restores any reusable
  context value afterward. Slice 80 adds the optional `RunTurnInputs::trace`
  context (`TraceRepository`, session id, parent turn id, agent key, origin,
  redacted context JSON) used by the first terminal-success writer; slice 82
  adds the explicit `enabled` gate; slice 83 writes cancelled trace rows using
  the same turn context; slice 84 writes ordinary provider and loop-boundary
  error rows; slice 85 generates missing trace turn ids.
- **Turn-finished publisher**. `agent::Loop::run_turn` writes one
  `trace_turns` row at terminal stop reason. The write is
  *synchronous* w.r.t. the user-visible response (the loop awaits
  the insert before returning) so the row is durable before the
  agent answers. The cost is one SQLite insert per turn (≤ 30 µs
  per the existing `bench-storage` numbers). **Status (slice 85):**
  shipped for trace-enabled terminal-success stop reasons (`end_turn`,
  `stop_sequence`, `max_tokens`) through `RunTurnInputs::trace`; explicit
  `TraceContext::enabled=false` skips the insert entirely. Parent-cancelled
  provider/tool failures also write `stop_reason=cancelled` rows after
  briefly shielding the insert from the parent cancellation state. Non-cancelled
  provider errors and response-backed loop-boundary failures write
  `stop_reason=error` rows while preserving the original returned error. The
  writer now generates a turn id when a trace repository is configured and the
  caller leaves `RunTurnInputs::turn_id` empty. Slice 86 also writes a
  `stop_reason=error` row when `LoopOptions::max_iterations` is exhausted by
  repeated tool_use responses; the row records the final iteration's prompt
  hashes/bytes, the aggregated provider usage, and the last response's model
  id (falling back to the primary route model) before the existing
  `Error::internal` (reason=`iteration_cap`) returns.
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
    "retention_days": 30        // trace-row purge window; existing 'retention_days' pattern
  }
  ```
  `enabled=false` skips the SQLite insert entirely (still emits
  audit rows; trace is the *joining* layer). **Status (slice 87):**
  `oran-config` parses this top-level block into `config::TraceConfig` with
  defaults `{enabled=true, store_raw_bodies=false, retention_days=30}` and
  validates boolean flags plus positive integer retention. `agent::Loop` now
  honors the equivalent explicit `RunTurnInputs::trace.enabled=false` policy
  by writing no trace row and preserving NULL audit parent ids. `bootstrap::run`
  threads `config.trace().enabled` into the new
  `RuntimeAssemblyOptions::trace_enabled`, and `RuntimeAssembly::build`
  constructs a `storage::TraceRepository` on the shared audit `Pool` whenever
  both audit and trace are enabled (`RuntimeAssembly::trace_repository()`
  exposes the pointer consumed by slice 101's `AgentPromptRunner` when a caller
  supplies a provider backend; audit-disabled forces the trace repository to
  stay null). Slice 150 has `bootstrap::run` convert
  `config.trace().retention_days` into an explicit Unix-nanosecond cutoff
  before assembly build; when tracing is enabled, the assembly applies that
  cutoff through `TraceRepository::purge_turns_started_before(...)` after the
  schema migration and before exposing the long-lived trace repository.
  `store_raw_bodies` still waits for the trace runtime that will consume it.
- **CLI surface**. `orangutan --trace <turn_id>` prints the row
  plus every joined audit row (`WHERE parent_turn_id = ?`) in the
  same `--explain-rules`-style table format that already exists for
  permission rules. The query is read-only; no permission changes.
  **Status (slice 88):** shipped. `oran-bootstrap` parses
  `--trace <hex>` / `--trace=<hex>` (32-char lowercase hex turn id), runs the
  idempotent audit migration, calls `TraceRepository::get_turn` for the trace
  row, joins audit rows via the new `AuditRepository::list_events_for_turn`
  (ordered `id ASC` so the original `tool_use` order is preserved), and
  renders both blocks to stdout before exiting `0`. The inspector returns
  `Error::not_found` for a missing audit DB and for an unknown turn id, and
  forwards SIGINT/SIGTERM through the existing `SignalScope` so it shares the
  `--audit-init` cancellation contract. Slice 93 adds
  `event_kind=hook_publish` rows to the same `list_events_for_turn`
  join and the inspector's audit-row output now prints
  `kind=<event_kind>`, so operators can tell hook publish rows from
  ordinary permission decisions in deterministic `id ASC` order.

## Scope (v1.1)

- **Trace export**. JSON Lines emitter for an external SIEM. One
  line per turn; trace row + joined audit rows + joined hook
  publishes. Output to stdout, file, or `oran-http` POST endpoint.
  **Status (slice 242):** stdout export covers both single-turn and bounded
  multi-turn operator reads, and the same JSON Lines sequence can be written to
  an explicit file sink or POSTed to an operator-supplied HTTP endpoint.
  `orangutan --trace-export <turn-id>` keeps the slice-239 behavior: the same
  32-character lowercase hex validation as `--trace`, idempotent audit
  migration, one `trace_turns` row, and joined `audit_events.parent_turn_id`
  matches emitted as one JSON Lines object with parsed `context_json` and
  `metadata_json` fields. `orangutan --trace-export [--agent <name>] [--limit
  <n>]` lists newest trace rows through `TraceRepository::list_turns`,
  optionally filters by `agent_key`, joins each turn's audit rows in `id ASC`
  order, and emits one `kind="trace_turn"` JSON Lines object per turn. Adding
  `--trace-export-file <path>` to either form creates parent directories,
  truncates the target file, writes the same redacted JSON Lines objects, and
  suppresses stdout. Adding `--trace-export-post <url>` instead sends the same
  newline-delimited JSON payload as `application/x-ndjson`, accepts 2xx
  responses, and reports non-2xx responses as IO errors with the status code.
  File and POST sinks are mutually exclusive.
- **Provider streaming phase rollup**. The `cancellation_phase`
  field grows to record `provider_stream`,
  `provider_initial`, `provider_complete` so a cancellation during
  streaming is distinguishable from one before the first byte.
  **Status (slice 244):** shipped in `agent::Loop`. Parent-cancelled provider
  awaits now return and persist `provider_initial` before any sink callback,
  `provider_stream` after text/thinking/tool deltas, and `provider_complete`
  after the provider terminal `on_done` callback.
- **Tool-call rollup** — derived view on `audit_events` that pre-
  aggregates per-turn tool counts, per-tool latencies, per-tool
  failure rates. Lives as a SQL view, not a new column.
  **Status (slice 243):** shipped as audit DB migration version 5 and
  `AuditRepository::list_tool_call_rollups(...)`. The current rollup groups
  permission-decision rows and sibling `hook_publish` rows by
  `parent_turn_id` + `tool_name`, returning decision/hook counts,
  permitted/blocked decision counts, and optional latency samples from valid
  `metadata_json.usage.wall_time_ms`. It deliberately does not claim complete
  handler failure rates yet because failed handler exits are not durable audit
  rows today.
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

- **A trace UI**. Desktop app consumption lives in spec 0007. v1 ships
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
   **Status (slice 80):** shipped for trace-enabled terminal-success turns and
   covered by `test-agent`'s single-text trace-row case.
2. **Cause-chain join.** A single-tool turn (spec 0017 scenario #2)
   produces one trace row + one audit row whose `parent_turn_id`
   matches the trace row's `turn_id`. The join query in the CLI
   inspector returns both. **Status (slice 85):** the storage join is shipped
   for direct dispatch when callers configure `RunTurnInputs::trace`; callers
   may provide `RunTurnInputs::turn_id`, and otherwise the loop generates one
   before dispatch. CLI inspection remains downstream.
3. **Multi-tool fan-out.** A multi-tool turn (spec 0017 scenario
   #3, sequential dispatch in v1) produces one trace row + N audit
   rows, all sharing the same `parent_turn_id`. Sorted by
   `started_at_ns`, the audit rows preserve the original
   `tool_use` order. **Status (slice 85):** direct sequential dispatch stamps
   every tool audit row with the same loop turn id, preserves the existing
   tool-use order at the loop boundary, terminal-success turns write the
   parent trace row, and missing turn ids are generated when trace is
   configured. Dedicated N-tool storage-join coverage remains downstream.
4. **Cancellation phase recorded.** Cancellation during provider
   await (spec 0017 scenario #9) produces a trace row with
   `cancellation_phase='provider_initial'`, `provider_stream`, or
   `provider_complete` depending on whether the provider had emitted no
   stream callbacks, visible deltas, or terminal `on_done`.
   Cancellation during tool dispatch (scenario #10) produces
   `cancellation_phase='tools'`. **Status (slice 244):** shipped for
   trace-enabled parent-cancelled provider and tool phases. The loop still
   returns `ErrorKind::cancelled` with `reason=parent_cancelled` and the same
   phase after the row is written.
5. **Hook publish observable.** A blocking `tool_before` veto
   (spec 0015) appends a `hook_publish` audit row with
   `parent_turn_id` matching the trace row; the row's
   `context.decision_kind='veto'` and `context.sink_id='<id>'` so
   the cause-chain shows *which sink* vetoed. **Status (slice 93):**
   shipped for direct dispatch. A traced blocking `tool_before` publish writes
   a `hook_publish` row before the permission decision row; storage tests cover
   audit schema version 4, event-kind filtering, and mixed
   `list_events_for_turn` ordering, and tool tests cover the joinable veto row
   with sink trace metadata.
6. **Token / cost rollup.** A turn whose provider response carries
   `Usage = { input_tokens: 1500, output_tokens: 200,
   cache_read_tokens: 4096, cost_estimate: 0.012 }` writes the
   same values into `trace_turns`. **Status (slice 129):** shipped for the
   loop writer and storage rollup reader. Agent tests pin per-turn writes for
   terminal, cancelled, error, iteration-cap, and profile-priced cost rows; the
   loop now computes a missing `cost_estimate` from the selected
   `ModelTarget::pricing` before trace rows are written, while preserving a
   provider-supplied cost when present. Storage tests pin
   `TraceRepository::list_provider_usage_rollups`, which sums input/output/cache
   tokens and existing `cost_estimate_usd` values by UTC day, agent key, route
   profile, and route model.
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
   is NULL when trace is off). **Status (slice 87):** shipped at both the
   `agent::Loop` input boundary for explicit
   `RunTurnInputs::trace.enabled=false` and the `RuntimeAssembly` boundary:
   `bootstrap::run` maps `config.trace().enabled` into
   `RuntimeAssemblyOptions::trace_enabled`, and the assembly only constructs
   a `storage::TraceRepository` when both audit and trace are enabled.
   The loop writes zero trace rows when `TraceContext::enabled=false`,
   direct-dispatch audit rows keep `parent_turn_id = NULL`, and any previous
   reusable dispatch-context parent id is restored after the tool call.
   Slice 101's `AgentPromptRunner` threads the assembly-owned repository into
   `RunTurnInputs::trace` when tests or future callers supply a provider
   backend. Slice 112 then wires `bootstrap::run` to use that runner through
   `HttpProviderBackend` for configured routes, so trace rows are now written
   from the ordinary binary path as soon as config declares a `default` route.
10. **CLI inspector.** `orangutan --trace <turn_id>` returns
    the trace row + every joined audit row + every joined
    `hook_publish` row in deterministic order; exit code 0.
    Unknown `<turn_id>` exits non-zero with `Error::not_found`.
    **Status (slice 93):** shipped for the trace row + joined audit rows,
    including `hook_publish` rows.
    `oran-bootstrap` parses `--trace <hex>` / `--trace=<hex>` (32-char
    lowercase hex matching the storage BLOB round-trip), uses the new
    `AuditRepository::list_events_for_turn(TurnId, limit)` to join audit
    rows ordered `id ASC` (preserving the original spec-0017 `tool_use`
    order), and renders the trace turn + audit rows in `--explain-rules`-
    style lines. Unknown turn id returns `Error::not_found`; missing
    audit DB returns `Error::not_found` with a path-pointing message. Slice 93
    adds `kind=<event_kind>` to each audit row line so the joined output is
    readable once hook-publish rows are present.
11. **Schema migration.** Migrating an existing `audit.db` from
    schema 1 to schema with the trace tables succeeds idempotently;
    re-running the migration is a no-op. Pinned by a migration
    test against a fixture from a pre-spec-0018 build.
12. **Insert cost.** `bench/oran-storage/trace_turn_insert` reports
    ≤ 50 µs per insert on the bench fixture, matching the existing
    `audit_event_append` numbers (~18 µs end-to-end through
    SQLite). The trace insert is one row plus the per-tool audit
    inserts the loop already pays for.
    **Status (slice 89):** shipped. `bench/storage/scenarios/trace_turn_insert.cpp`
    registers a per-insert A-vs-B pair against `trace_turns`:
    `storage.trace_turn_insert_raw_pool` (raw `Pool` + `StatementCache`,
    one row per nanobench iteration) and
    `storage.trace_turn_insert_repository` (`TraceRepository::append_turn`,
    one row per iteration). Initial WSL2 numbers report about
    13 µs / insert for the raw path and about 16 µs / insert for the
    repository wrapper -- both inside the spec target. The existing
    `scenarios/trace_repository.cpp` (32-row batch) needed an
    `id_for` collision fix in the same slice because the original
    overlapping-sum encoding broke the trace PRIMARY KEY guard the
    moment nanobench advanced past the first epoch; both batch
    scenarios now run alongside the new single-insert pair.
13. **Trace JSONL export.** A known `<turn_id>` passed to
    `orangutan --trace-export <turn_id>` prints exactly one JSON
    Lines object with `kind="trace_turn"`, the trace row, and every
    joined audit row in deterministic `id ASC` order. Unknown ids
    return `Error::not_found`; malformed ids use the same validation
    as `--trace`.
    **Status (slice 242):** shipped for single-turn and bounded multi-turn
    stdout export plus explicit file and HTTP POST sinks. `test-bootstrap` pins
    empty-value and invalid-limit handling, unknown single-turn ids, mutual
    exclusion with `--trace`, parsed trace context JSON, parsed hook-publish
    metadata JSON, NULL input hashes, the one-line single-turn output shape,
    the multi-line newest-first agent-filtered list shape,
    success-with-empty-output for a bounded list query with no matches, file
    output for both single-turn and bounded list modes, stdout suppression when
    a file or POST sink is selected, duplicate/missing/empty/unscoped sink
    rejection, file/POST mutual exclusion, loopback POST payload shape for
    single-turn and bounded list modes, and non-2xx POST failure reporting.
14. **Tool-call rollup.** A traced turn with several permission-decision rows
    for multiple tools and sibling `hook_publish` rows can be queried as
    per-turn/per-tool rollups without scanning raw audit rows in the caller.
    The rollup reports decision counts, hook-publish counts,
    permitted/blocked decision counts, and averages only valid
    `metadata_json.usage.wall_time_ms` samples. Rows without a
    `parent_turn_id` are excluded, and the storage API supports a parent-turn
    filter, a tool-name filter, and a bounded limit.
    **Status (slice 243):** shipped. Audit DB migration version 5 creates
    `audit_tool_call_rollups`, and `AuditRepository::list_tool_call_rollups`
    exposes the derived rows. `test-storage` pins migration v5 application,
    the view's presence, invalid JSON handling, hook/decision aggregation,
    filter/limit behavior, and malformed option validation.

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
xmake run test-storage                        # trace_turns migration + insert + query
xmake run test-storage "[audit_repository]"   # audit join + tool-call rollups
xmake run test-agent                          # trace rows joined to spec 0017 scenarios
xmake run test-tool                           # direct tool_before hook_publish rows
xmake run test-bootstrap                      # --trace inspector output over mixed rows
xmake run test-bootstrap "[trace]"            # --trace + --trace-export paths
xmake build bench-oran-storage
xmake run bench-oran-storage trace_turn_insert
xmake run orangutan -- --trace <turn-id>      # CLI inspector
xmake run orangutan -- --trace-export <turn-id> # JSON Lines trace export
xmake run orangutan -- --trace-export --agent coder --limit 10
xmake run orangutan -- --trace-export --agent coder --limit 10 --trace-export-file traces.jsonl
xmake run orangutan -- --trace-export --agent coder --limit 10 --trace-export-post http://127.0.0.1:9000/traces
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

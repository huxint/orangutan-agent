# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now. Update in the same commit as
> the history entry that moves it — see
> [`rules/docs-in-sync.md`](rules/docs-in-sync.md).

## Snapshot

- **Slice:** 85 (`xmake run orangutan` reports slice 85)
- **Last completed history:**
  [`histories/2026-05/20260524-1325-agent-trace-turn-id-generation.md`](histories/2026-05/20260524-1325-agent-trace-turn-id-generation.md)
- **Active exec-plan:** none — the prompt-builder skeleton plan remains
  archived at
  [`exec-plans/completed/2026-05-23-prompt-builder-v1.md`](exec-plans/completed/2026-05-23-prompt-builder-v1.md);
  the recent agent-loop increments closed in focused history/commit slices
  and did not need a new plan because they stayed under the existing spec-0017
  sequencing contract.
- **Next intended slice:** Continue along the spec dependency graph
  (0013 → 0011 + 0012 → 0014 → 0016 → 0017 → 0015 → 0018). Slice 85 lands
  loop-owned trace turn-id generation for spec-0018. When
  `RunTurnInputs::trace` has an enabled `TraceRepository` and the caller does
  not provide `RunTurnInputs::turn_id`, `agent::Loop` now generates a non-zero
  version/variant-shaped 16-byte turn id before the first prompt render. The
  generated id is used for the `trace_turns.turn_id` row and for every direct
  `audit_events.parent_turn_id` stamped during that turn, preserving the
  trace/audit join without requiring test-only or future bootstrap callers to
  pre-fill the id. Trace-disabled and repository-less pre-trace callers still
  keep `parent_turn_id = NULL` unless they explicitly supply a turn id.
  `test-agent` now reports 22 cases / 345 assertions. Config-to-loop wiring,
  hook publish rows, iteration-cap trace rows, CLI `--trace`, and binary
  handoff remain downstream. Slice 84 lands the
  first ordinary error trace rows for spec-0018's loop-owned writer.
  `agent::Loop` now writes a durable `trace_turns` row with
  `stop_reason=error` before returning a non-cancelled provider error, using
  the primary route model because no provider response exists. It also writes
  `stop_reason=error` rows for response-backed loop-boundary failures:
  `tool_use` responses without caller-supplied dispatch services,
  `tool_use` stop reasons without tool blocks, unsupported non-terminal stop
  reasons, and non-cancelled storage/internal direct-dispatch failures. Those
  rows use the provider response model when present and preserve the aggregate
  usage observed before the error. Parent-cancelled provider/tool paths still
  take the slice-83 `cancelled` writer and do not perform any extra await while
  a terminal cancellation is active unless the cancellation row path has first
  reset the coroutine cancellation state. Config-to-loop wiring, hook publish
  rows, CLI `--trace`, and binary handoff remain downstream. Slice 83 lands the
  first cancellation trace rows for spec-0018
  AC4. `agent::Loop` now writes a durable `trace_turns` row with
  `stop_reason=cancelled` and `cancellation_phase=provider|tools` when parent
  cancellation lands during the provider await or direct tool dispatch and
  `RunTurnInputs::trace` is enabled. The writer briefly resets the coroutine
  cancellation state only for the trace insert so the audit row can survive the
  cancellation that caused it; the returned error remains
  `ErrorKind::cancelled` with `reason=parent_cancelled`. Provider-phase rows
  use the primary route model because no provider response exists; tool-phase
  rows use the provider response model and aggregate usage observed before the
  tool cancellation. Slice 82 lands the
  explicit trace-disabled loop policy required by spec-0018 AC9.
  `agent::TraceContext` now has an `enabled` switch that defaults to true for
  existing trace-enabled and pre-trace callers. When callers set
  `RunTurnInputs::trace.enabled=false`, `agent::Loop` writes zero
  `trace_turns` rows even if a `TraceRepository` is present, threads
  `std::nullopt` into direct tool dispatch so new audit rows keep
  `audit_events.parent_turn_id = NULL`, and restores any reusable
  `tool::DispatchContext::parent_turn_id` after the dispatch finishes.
  `test-agent` covers the policy with a storage-backed single-tool turn.
  Bootstrap still does not map
  `config::TraceConfig` into loop inputs; hook publish rows, CLI `--trace`,
  and binary handoff remain downstream. Slice 81 lands the
  typed operator trace policy surface: `config::TraceConfig` and
  `Config::trace()` parse the top-level `trace.enabled`,
  `trace.store_raw_bodies`, and `trace.retention_days` block documented by
  spec 0018, with defaults `{true, false, 30}` and config-time validation for
  boolean flags plus positive integer retention. `config.example.json` carries
  the default block, and `test-config` covers custom values, the example file,
  and malformed trace policy (30 cases / 225 assertions). The parsed config is
  still not wired through bootstrap; slice 82 adds the equivalent explicit loop
  switch on `RunTurnInputs::trace`, and trace rows still require the
  caller-supplied trace context from slice 80. Slice 80 lands the
  first loop-owned spec-0018 `trace_turns` writer for terminal-success fake
  provider turns. `agent::RunTurnInputs::trace` carries a non-owning
  `storage::TraceRepository*`, `session_id`, optional `parent_turn_id`,
  `agent_key`, `origin`, and redacted `context_json`; when callers also supply
  `RunTurnInputs::turn_id`, or slice 85 generates one for a configured trace
  writer, `agent::Loop` awaits one `TraceRepository::append_turn` before
  returning terminal `end_turn` / `stop_sequence` / `max_tokens` results.
  The row records route profile/model, start/finish timestamps, stop reason,
  iteration count, prompt prefix hash/bytes, active/deferred catalog hashes,
  aggregate provider usage tokens/cost, cache token counters, and body-free
  context bytes. The existing direct-dispatch audit path still stamps
  `audit_events.parent_turn_id` with the same turn id, so a single-tool loop turn
  now has both sides of the cause-chain join. `test-agent` covers single-text
  trace rows, storage-backed tool-audit correlation, the slice-82 disabled
  policy case, the slice-83 provider/tool cancellation trace rows, and the
  slice-84 provider/loop-boundary error trace rows, and the slice-85 generated
  turn-id trace/audit join path (22 cases / 345 assertions). Iteration-cap
  trace rows, config-to-loop wiring, hook publish rows, CLI `--trace`, and
  binary handoff remain downstream.
  Slice 79 threads the first spec-0018 cause-chain id through the direct
  tool-dispatch path. `oran-core` now owns `core::TurnId`, the shared 16-byte
  value shape used by storage trace ids and audit correlation. `storage::TraceId`
  aliases it; audit DB migration
  `src/oran-storage/migrations/audit/0003-audit-parent-turn-id.sql` adds the
  nullable `audit_events.parent_turn_id` BLOB plus an index, so the embedded
  audit/trace migration stream now reaches version 3. `AppendAuditEventRequest`,
  `UpdateAuditEventMetadataRequest`, `AuditEventRecord`,
  `permission::AuditEvent`, and `AuditMetadataUpdate` all expose optional typed
  `parent_turn_id`; `StorageAuditSink` persists it; and same-row metadata
  enrichment matches it so concurrent same-tool calls from different turns do
  not clobber each other. `tool::DispatchContext` carries the optional parent
  turn id into `Registry::dispatch`, and `agent::RunTurnInputs::turn_id` is the
  loop-owned source for direct tool calls: traced turns stamp every dispatch
  with that id, while explicit trace-disabled turns force `parent_turn_id = NULL`
  during dispatch and restore any reusable context value afterward. `test-core` covers
  the value type (70 cases / 453 assertions), `test-storage` covers audit
  version-3 migration, BLOB round-trip, metadata update scoping, and zero-id
  validation (70 cases / 856 assertions), `test-permission` covers
  recording/storage sink propagation (89 cases / 414 assertions), and
  `test-tool` covers registry audit stamping (166 cases / 1590 assertions).
  Slice 78 opened the
  storage foundation for spec 0018: `oran-storage` exports
  `TraceRepository`, `TraceId` (16-byte BLOB at the database boundary),
  `AppendTraceTurnRequest`, `TraceTurnRecord`, `ListTraceTurnsOptions`, and
  `built_in_trace_migrations()`. Slice 77 extends
  the real `agent::Loop` driver from the slice-76 sequential tool loop into
  the first cancellation-phase classification needed by specs 0017/0018.
  Provider-await cancellations and tool-dispatch cancellations still return
  `ErrorKind::cancelled`, but the loop now adds
  `reason=parent_cancelled` plus `cancellation_phase=provider|tools` before
  returning the error; slice 83 also writes matching cancelled trace rows when a
  trace context is configured. Slice 78 introduced the trace schema and
  repository, and slice 80 wires terminal-success rows.
  Ordinary provider errors, retryable network/upstream failures, storage
  failures, and model-repairable tool errors keep their existing return
  behavior; trace-enabled turns now record provider and response-backed
  loop-boundary failures as `stop_reason=error`.
  `test-agent` covers both cancellation phases through parent
  `asio::cancellation_signal` tests.
  Iteration-cap trace rows, blocking approval rendering, provider retry/fallback, the
  parallel `ToolScheduler`, and CLI/binary handoff remain downstream. Slice 76
  extended the real `agent::Loop` driver from the slice-75 text-only path into the
  first sequential direct-dispatch tool loop. `<oran/agent.hpp>` exports
  `agent::Loop`, `LoopOptions`, `RunTurnInputs`, and `RunTurnResult`;
  `RunTurnInputs` can now carry optional non-owning `tool::Registry*` and
  `tool::DispatchContext*` pointers. When both are present and the provider
  returns `ToolUseContent`, the loop appends the assistant tool-use message,
  dispatches each tool through the existing registry boundary in original
  tool-use order, appends a `Role::tool` message with ordered
  `ToolResultContent` blocks, rebuilds the seven-section prompt from the
  updated transcript, and sends the next `provider::Request` through the same
  `provider::System` / `provider::Route`. It aggregates provider usage across
  iterations, returns the terminal assistant text/blocks/model id, final
  rendered prompt/cache hints, `iterations`, and the complete transcript tail
  including the final assistant answer. Missing tools and model-repairable
  dispatch errors are converted into `tool_result` error blocks so the model can
  repair; cancellation, storage, and internal dispatch errors propagate out of
  the loop. If the registry/context pair is absent, `tool_use` still returns
  the explicit not-yet-implemented error from slice 75. The loop enforces the
  existing `LoopOptions::max_iterations` cap with `reason=iteration_cap`, but
  iteration-cap trace rows, provider retry/fallback, blocking approval
  rendering, and the parallel `ToolScheduler` remain downstream. `test-agent`
  now covers the FakeProvider text-turn path,
  provider request mapping, provider error forwarding, the no-dispatch-context
  tool-use boundary, one-tool provider re-entry, ordered multi-tool results,
  model-visible missing-tool repair, infrastructure error propagation, and the
  iteration cap, provider/tool cancellation trace rows, and provider/loop-boundary
  error trace rows. The `orangutan` binary is still not wired to `oran-agent`;
  remaining near-term work is the approval-observability envelope before
  CLI/binary handoff.
  Slice 75 opened
  the real `agent::Loop` driver but deliberately limited it to spec-0017
  scenario #1 and request-mapping boundaries. `<oran/agent.hpp>` began
  exporting `agent::Loop`, `LoopOptions`, `RunTurnInputs`, and
  `RunTurnResult`; the loop owned a `prompt::Builder`, built the seven-section
  prompt from caller-supplied stable inputs and the conversation tail, mapped
  the rendered prefix into `provider::PromptCacheHints`, mirrored the prompt
  active/promoted tool set into deterministically name-sorted
  `provider::Request::tools`, sent one `provider::Request`, and returned
  terminal text-style responses while loudly rejecting `tool_use`.
  Slice 74 closes
  spec 0017's provider prework: `oran-provider` now exports the abstract
  `provider::System` (single `send(Request, Route, EventSink*) const`
  entry), the `provider::EventSink` streaming observer with default no-op
  callbacks for text/thinking/tool deltas plus terminal `on_done`, the
  `ProtocolKind` / `ModelTarget` / `Route` value shapes the loop will
  resolve once per turn, and `provider::FakeProvider` — the first concrete
  `System` — with a `ScriptedTurn` / `StreamDelta` plan, plan-exhaustion
  guard, cancel-aware scripted latency through `async::sleep_for`, and a
  delta-to-`Response` assembler that fans the same calls out to the
  observer. `oran-provider` now depends on `oran-async` (the layer-1
  platform dep already used by `oran-prompt`). `test-provider` covers
  the canned-response path, delta assembly with text+tool blocks, scripted
  error injection, plan exhaustion, empty-turn rejection, multi-turn
  drive, null-sink tolerance, and parent cancellation during scripted
  latency. The provider library is still not linked into the `orangutan`
  binary and does not yet contain a real transport, protocol adapter,
  retry runtime, or vendor cache-control mapping. Remaining near-term
  work at that point was the `agent::Loop` MVP; slice 75 opened the
  text-only subset and slice 76 added sequential direct-dispatch
  provider re-entry, while provider adapter mapping remains downstream. Slice 73
  opens `oran-provider` with the adapter-facing cache-hint surface needed
  between spec 0016 and the fake-provider-first loop. `<oran/provider.hpp>`
  now exports provider-domain `Request`, `Response`, `Usage`, `RetryPolicy`,
  `PromptCacheHints`, `PromptCacheOptions`, and
  `make_prompt_cache_hints(RenderedPrompt, options)`. The mapper validates
  the prompt-design boundary (`RenderedPrompt::sections` has exactly seven
  sections, exactly one breakpoint, and that breakpoint is section 6 before
  the conversation tail), checks `prefix_bytes` against the actual section
  bytes, maps sections 1-6 into `(id, content_hash, cache_version)` cache
  keys plus the prefix hash/byte count, excludes `conversation_tail`, and
  supports route-level cache disable / minimum-prefix skip. `test-provider`
  covers successful prefix-only mapping, disable/size-floor skips, and
  malformed boundary rejection; `bench-provider` compares
  `provider.cache_hints_enabled` at about 394 ns / mapping with the disabled
  route at about 317 ns / mapping. Slice 73's surface is the prerequisite the
  slice-74 fake-provider foundation consumes; together they enabled the
  slice-75 text-turn `agent::Loop` foundation; slice 76 consumes the same
  provider contract for the first sequential tool-dispatch scenario matrix.
  Slice 72
  opens `oran-agent` with the narrow session-state owner needed by spec
  0016 before the full ReAct loop lands. `agent::SessionState` owns
  `prompt::PromotionState`, observes successful `tool.search` outputs,
  parses their structured `{kind:"tool_search", matches[]}` payload in a
  private `nlohmann_json` TU, promotes only deferred match names into the
  next prompt snapshot, ignores non-search and failed-search outputs, and
  returns `ErrorKind::invalid_argument` for malformed successful structured
  data without mutating state. `test-agent` covers promotion into the next
  prompt, no-op non-search / failed-search outputs, and malformed successful
  payload rejection; `bench-agent` now runs the agent-owned prompt-cache
  fixture (`agent.prompt_cache_no_promotions` about 54.4 us / fixture,
  `agent.prompt_cache_after_promotion` about 63.1 us / fixture) and aborts
  if `RenderedPrompt::prefix_hash` drifts across changing conversation tails.
  `oran-agent` is not linked into the `orangutan` binary yet and does not
  contain the fake-provider ReAct loop. Slice 71
  extended `oran-prompt` with `prompt::PromotionState`, a session-owned
  value type for deferred-tool promotions, and taught `prompt::Builder` to
  consume sorted promotion snapshots. Slice 70 opened the `prompt::Builder`
  skeleton, slice 69 landed the typed `runtime.prompt.active_tools` config
  surface, and slice 68 landed the registry-owned non-deferred
  `tool.search` lookup primitive.
  Slice 67
  closes spec 0014's audit usage fan-out for the pre-scheduler direct
  dispatch path: `permission::AuditSink` now exposes
  `update_metadata(AuditMetadataUpdate)`, `RecordingAuditSink` and
  `StorageAuditSink` implement it, and `storage::AuditRepository` can
  replace the newest matching `audit_events.metadata_json` value without
  appending a second permission-decision row. `Registry::dispatch` still
  records the permission decision before any handler side effects; when an
  allow or ask-approved handler returns a successful `tool::Output`,
  dispatch applies output caps, serializes non-empty `Output::usage` under
  `metadata_json.usage`, and best-effort enriches the same audit row. The
  direct-dispatch enrichment covers the shipped filesystem built-ins and
  the cap flags from slice 66. Provider adapter mapping remains the
  remaining spec-0014 item; scheduler ownership of cap options and any
  stronger per-batch audit correlation belong to the upcoming spec-0012 /
  agent-loop work. Slice 66
  closes spec 0014's byte-cap item for the pre-scheduler dispatch
  boundary: `<oran/tool/output.hpp>` now exposes
  `OutputCapOptions`, `OutputCapReport`, and `apply_output_caps`, and
  `Registry::dispatch` applies `DispatchContext::output_caps` to
  successful handler output before returning it or publishing
  `tool_after`. Text overflow is truncated on a UTF-8 code-point
  boundary and sets `usage.truncated`; structured-data overflow drops
  only `data_json` and sets `usage.data_dropped`, leaving the text
  fallback intact. `oran-config` now parses the documented
  `runtime.tool_output.max_text_bytes` / `max_data_bytes` block
  (defaults 256 KiB / 1 MiB) so the future scheduler/agent owner can
  thread operator caps into `DispatchContext` instead of hard-coding
  them. Provider adapter mapping remained downstream, and slice 67 adds
  audit usage metadata enrichment. Slice 65
  closes spec 0014's hook raw-data redaction item: `hook::Sink` now
  exposes `kind()` with `SinkKind::default_` and
  `SinkKind::trusted_local`, `hook::InProcessSink` stores the chosen
  kind, `ToolAfterPayload` can carry optional raw structured
  `data_json`, `Registry::dispatch` copies successful
  `Output::data_json` into the hook payload, and
  `Bus::publish_advisory` clears that field for every sink that is not
  `trusted_local`. Default sinks therefore keep text + usage only, while
  trusted-local observers can receive the raw structured bytes. With
  `file.read` (slice 62), `file.search` (slice 63), and
  `directory.list` (slice 64) migrated and the mutation tools holding
  measured usage counters from slice 61, the built-in side of spec 0014's
  structured-output migration is done. Provider adapter mapping,
  byte-cap enforcement, and audit usage fan-out were downstream at that
  point; later slices shipped byte caps and same-row audit usage metadata
  enrichment.
  Slice 64
  continues spec 0014's built-in structured-output migration for
  `directory.list`: the handler keeps the existing
  `<path>:<kind>:<size_bytes or '-'>` text rendering, now fills
  `Output::data_json` with a `{kind:"directory_list", path,
  include_hidden, max_entries, entry_count, entries[]}` payload
  (each entry carries `{name, path, kind, size_bytes}` with JSON null
  for non-regular kinds), and fills `Output::usage.files_touched=1`
  plus `match_count=entry_count` so audit fan-out can see directory-walk
  cost without parsing prose.
  Slice 63 migrated `file.search`: the handler keeps the existing
  `path:line:text` text rendering (with the slice-47 byte-cap and
  slice-20 match-cap trailing summary), now fills `Output::data_json`
  with a `{kind:"file_search", path, pattern, regex, matches[],
  match_count, truncated, truncation_reason, files_scanned,
  bytes_read}` payload, and fills `Output::usage.bytes_read`
  (cumulative scanned file bytes), `files_touched` (non-binary scanned
  file count), `match_count` (post-truncation match count), and the
  `truncated` cap flag. Slice 62
  continues spec 0014's built-in structured-output migration for
  `file.read`: the tool keeps the spec-0011 text header/body fallback,
  now fills `Output::data_json` with a JSON object carrying `kind`,
  `path`, requested `text`, `fingerprint`, `start_line`, `end_line`,
  `returned_bytes`, and `truncated`, and fills `Output::usage.bytes_read`,
  `files_touched`, and `truncated`. Slice 61 moved the current mutation
  tools onto usage counters: `file.write` fills
  `Output::usage.bytes_written` and `files_touched`; `file.edit` fills
  `bytes_read`, `bytes_written`, `files_touched`, and `match_count`; and
  `file.delete` fills `bytes_written=0` plus `files_touched=1`. The
  mutation tools keep their existing text summaries and leave `data_json`
  empty for the v1 migration path. Slice 60
  closed the deep-review "tool output is too small" finding and started
  spec 0014 inside `oran-tool`: `tool::Output` now lives in
  `<oran/tool/output.hpp>` with required `text`, optional serialized
  `data_json`, attachment metadata, usage counters, and `is_error`;
  `Output::text_only` preserves the v1-compatible text path, and
  `Output::error` can carry serialized structured error data. The public
  header stays `nlohmann`-free by storing structured payload bytes as a
  string for provider adapters to parse/serialize later. `Registry::dispatch`
  now copies `Output::usage` into `hook::ToolAfterPayload::usage` on
  successful handler returns. Provider-adapter mapping remains downstream;
  later slices migrated `file.search` / `directory.list` structured
  `data_json`, shipped trusted-local hook raw-data redaction, added the
  slice-66 dispatch-boundary output-cap helper, and added slice-67
  same-row audit usage metadata enrichment. Slice 59
  starts the prompt-catalog cache prework shared by specs 0012 and
  0016: `core::ToolDef` now carries the documented `deferred` and
  `category` metadata, and `oran-tool` exposes `tool::CatalogRenderer`,
  a single-strand deterministic renderer that sorts catalog snapshots by
  tool name, renders non-deferred tools as canonical schema blocks,
  renders deferred tools as name/description index rows, and memoises
  full-schema blocks in a bounded 256-entry cache keyed by stable
  rendered-block fields plus renderer version. The public stats report
  aggregate cache counters only. This is not yet the `oran-prompt`
  builder or promotion-set slice; slice 68 adds the registry-local
  `tool.search` lookup primitive that this renderer's future prompt
  builder will advertise as an active tool, and slice 69 adds the typed
  config surface that will select the active set.
  Slice 58
  closes spec 0011 v1.1's IO-layer watcher item: `oran-io` now exposes
  `watch_read_text_file_ranged_cache(executor, root, options)`, a
  cancel-aware Linux/inotify watcher that registers one directory or a
  recursive tree, drains filesystem events through an asio descriptor,
  and calls `invalidate_read_text_file_ranged_cache(path)` so external
  edits evict the affected file-view and line-offset-index entries
  without exposing cache keys. The returned `ReadTextFileWatchStats`
  reports only aggregate directories/events/invalidations. The watcher
  is not yet automatically started by bootstrap/config; that wiring waits
  for the runtime service that will own long-lived background tasks.
  Slice 57 landed the path-stale invalidation seam this watcher consumes:
  `core::BoundedCache` now has `erase_if(predicate)` for explicit
  non-policy invalidation, and successful `io::write_text_file` and
  `io::delete_file` reuse the same seam instead of clearing unrelated
  read-cache entries.
  Slice 56 closes spec 0012's approval-grant bounded-state item inside
  `oran-permission`: `ApprovalBroker::approve` now lazily reaps expired
  grants and keeps at most
  `ApprovalBroker::max_grants_per_identity` (64) live grant entries per
  identity, evicting the oldest same-identity grant when a new distinct
  `(tool, identity, input_hash)` triple would exceed the ceiling. Evicted
  tokens still verify cryptographically, but `ApprovalBroker::check`
  returns `reason=no_grant`. Slice 55
  closes spec 0013's v1 structural path-policy work inside `oran-tool`:
  `Registry::dispatch` now pre-resolves known filesystem built-in `path`
  inputs through `tool::Workspace` before permission evaluation, carries
  the absolute path to handlers via `DispatchContext::resolved_path`, and
  writes redacted resolver metadata (`input_path_hash`,
  `resolved_relative_path`, `workspace_root_hash`, symlink / parent /
  override flags, and resolver error kind/reason) under the existing
  `permission::AuditEvent::metadata_json` column. Path-policy failures are
  audited with the permission verdict but return before handlers run and
  before ask-approval replay is spent. Slice 54
  completed the public bounded-state observability surface for
  `oran-io`'s range-read caches: `read_text_file_ranged_cache_stats()`
  snapshots the private line-offset index and file-view cache
  `core::BoundedCache` counters (hits, misses, LRU/TTL/byte evictions,
  oversize rejections, current entries, current bytes) without exposing
  cache keys or paths. Slice 53's
  `read_text_file_ranged_singleflight_stats()` remains the paired
  in-flight-table snapshot.
  Spec 0013's remaining work is no longer v1 confinement plumbing; it is
  the v1.1 shared ignore predicate / display-helper work that waits for
  a second recursive consumer such as `directory.scan`, plus the future
  capability-gated `tool::Runtime::workspace()` accessor when
  `tool::Runtime` lands. The first
  provider adapter (Anthropic Messages) remains a multi-slice
  effort that needs an exec plan plus `oran-http` + libcurl wiring
  first; blocking hook semantics with veto are still gated on
  `oran-agent`; and wiring `check-compile-budget.sh` into
  `scripts/ci.sh` remains gated by the slice-28 reference-hardware
  precondition. The current `file.delete` and `directory.list`
  shapes are expected to be re-shaped in a later refactor: one
  unified delete tool covering both files and folders, and a
  recursive whole-project list (not just single-level children).
  Future built-in slices should not double down on per-kind splits
  like `directory.remove` or single-level enumeration.

## Library Health

Lifted from [`QUALITY_SCORE.md`](QUALITY_SCORE.md). `STATUS.md` summarizes;
`QUALITY_SCORE.md` explains.

| Score | Areas |
| ----- | ----- |
| **A** | *(none yet — pre-v1)* |
| **B** | Architecture docs, Build system, Async model, Security defaults, Supply chain |
| **C** | Compile-time discipline, Tests, Benches, IO, Storage, Config, Bootstrap, Provider system, Tool registry, Prompt builder, Memory tiers, Permissions, Hooks, Channels, Orchestration, Automation, Web UI, CLI, Static analysis |
| **D** | Skills, Observability |

## Latest Library Surfaces

- `oran-core`: 69 cases / 450 assertions.
- `oran-async`: 9 cases / 43 assertions.
- `oran-io`: 49 cases / 286 assertions.
- `oran-storage`: 68 cases / 827 assertions.
- `oran-config`: 28 cases / 207 assertions.
- `oran-permission`: 88 cases / 403 assertions.
- `oran-hook`: 17 cases / 109 assertions.
- `oran-tool`: 166 cases / 1588 assertions.
- `oran-prompt`: 10 cases / 98 assertions.
- `oran-provider`: 11 cases / 89 assertions.
- `oran-agent`: 14 cases / 154 assertions.
- `oran-cli`: 5 cases / 30 assertions.
- `oran-bootstrap`: 48 cases / 173 assertions.

## Open Tech-Debt Rows

Lifted from [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md).
Closed entries do *not* live here — the tracker is canonical.

- 2026-05-21 — Deep-review backlog: the stale root review artifact was
  deleted after its actionable findings were absorbed into the tracker and
  specs 0011-0018. Slices 31-36 closed the rank-0 items plus the P0
  follow-ups, and slice 60 closed the P2 `tool::Output` envelope item;
  remaining follow-ups are grouped P1/P2/P3 in the tracker.
- 2026-05-20 — `scripts/check-compile-budget.sh` exists and works (slice 28)
  but is not wired into `scripts/ci.sh`. Gated on CI provisioning xmake on
  the documented reference hardware (8-core / NVMe / native Linux);
  otherwise the gate fires on environmental drift, not real regressions.
- 2026-05-18 — Hook bus dispatch is advisory-only (slice 22); blocking
  semantics with veto for `tool_before` / `memory_*_before` /
  `permission_ask_rendered` are deferred to a follow-up slice.
- 2026-05-17 — `file.search` does not yet ship ripgrep-class optimisations
  (mmap, extension-based binary skip, multi-threaded walk).
  Adequate at slice 20 (~27 µs / 4-file tree) but 3-10× slower than a tuned
  scanner on repo-scale inputs. Re-bench once `oran-agent` produces a real
  workload measurement.
- 2026-05-17 — `scripts/check-prompt-preamble` static grep promised in
  `rules/prompt-design.md` not yet implemented (waits on first stable
  preamble template in `oran-agent`).
- 2026-05-14 — Generated `docs/generated/config.schema.json` not yet
  implemented.
- 2026-05-14 — bench A-vs-B scenarios listed in
  `bench/<lib>/README.md` are placeholders.
- 2026-05-14 — Frontend stack choice (Preact vs. plain JS) not yet
  decided.

## How To Update

1. The slice that lands a behavior change writes its history file.
2. The **same commit** updates this file: bump `Slice`, point
   `Last completed history` at the new file, refresh
   `Active exec-plan` (path or `none` + reason — see
   [`PLANS_GUIDE.md`](PLANS_GUIDE.md) "When NOT To Create A Plan"),
   refresh the test/assertion counts in "Latest Library Surfaces",
   and re-sync the tech-debt list from
   `exec-plans/tech-debt-tracker.md`.
3. `scripts/check-status-fresh.sh` fails the build if `STATUS.md`'s
   `Last completed history` pointer is older than the newest file under
   `docs/histories/`.

## See Also

- [`QUALITY_SCORE.md`](QUALITY_SCORE.md) — the per-area scoring rubric.
- [`releases/feature-release-notes.md`](releases/feature-release-notes.md)
  — chronological user-visible change log.
- [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md)
  — open debt rows.

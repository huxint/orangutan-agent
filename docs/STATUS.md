# Current State

> **One-screen project snapshot.** Read this *first* in every new session to
> orient on where the project is right now. Update in the same commit as
> the history entry that moves it — see
> [`rules/docs-in-sync.md`](rules/docs-in-sync.md).

## Snapshot

- **Slice:** 188 (`xmake run orangutan -- --help` reports slice 188)
- **Last completed history:**
  [`histories/2026-06/20260607-0424-bootstrap-retention-job.md`](histories/2026-06/20260607-0424-bootstrap-retention-job.md)
- **Active exec-plan:**
  [`exec-plans/active/2026-06-07-automation-retention-cadence.md`](exec-plans/active/2026-06-07-automation-retention-cadence.md).
- **Latest completed slice:** slice 188 maps configured-route
  `memory.longterm.retention` into the automation-owned
  `MemoryRetentionJob` descriptor without starting a scheduler. Bootstrap now
  exposes `longterm_memory_retention_job_from(...)`, derives startup decay
  options from that same job, sets the descriptor's `first_fire_at` to startup
  decay time plus `decay_check_interval_hours`, and passes the descriptor
  through `RuntimeAssemblyOptions::longterm_memory_retention_job`.
  `RuntimeAssembly` stores and exposes it through
  `longterm_memory_retention_job()` for diagnostics and future scheduler
  ownership, but does not evaluate, persist, lease, or run it. Focused result:
  `test-bootstrap` **124 cases / 1054 assertions**.
- **Next intended slice:** continue the active automation-retention cadence
  plan with a product-capability slice, not a bench-only slice. The next useful
  boundary is persistent periodic job/run state for the stored retention
  descriptor, while still avoiding a hidden bootstrap background loop until the
  service owner exists.

Slice 183 adds the operator-facing long-term
  retention policy contract. `oran-config` now parses
  `memory.longterm.retention.forget_after_unused_days`,
  `importance_floor`, `max_records_per_scope`, and
  `decay_check_interval_hours`, defaults them to 180 days / 0.0 / 10000 / 24 h,
  rejects malformed values, and preserves strict/loose unknown-field behavior
  for the nested retention block. Slice 184 consumes that policy for one
  configured-route startup decay pass; persistent periodic execution remains
  downstream.
  Slice 185 exposes the startup decay shadow count on the assembly and startup
  banner, slice 186 publishes the startup pass as advisory `memory_decay`
  metadata for build-time observers, and slice 187 gives the future periodic
  owner a pure `oran-automation` cadence/request planner for the same
  `DecayRequest` shape. Slice 188 maps the configured retention policy into the
  automation-owned periodic job descriptor and stores that scheduler seed on
  `RuntimeAssembly`.
  Focused result: `test-config` **48 cases / 429 assertions**.

Slice 182 adds the library-level long-term decay
  execution boundary. `memory::longterm` now exposes `DecayRequest`,
  `DecayResult`, and `Backend::decay(...)`; the default `Fts5Backend` marks
  scoped, visible records whose `last_read_at < unused_before` and
  `importance <= importance_floor` as `shadow=true` in bounded batches, updates
  the FTS shadow metadata, returns the shadowed records, and leaves default
  search excluding those rows unless `Query::include_shadow=true`. Pure
  automation cadence planning landed in slice 187, and bootstrap job-descriptor
  mapping landed in slice 188; persistent periodic execution and periodic decay
  publishing remain downstream. Startup hook publishing landed in slice 186.
  Focused result:
  `test-memory` **38 cases / 841 assertions**.

Slice 181 adds read-touch metadata to successful
  long-term recall. `memory::longterm` now exposes `TouchRequest` plus
  `Backend::touch(...)`; the default `Fts5Backend` advances `last_read_at`
  monotonically without rebuilding indexed text or changing `updated_at`, and
  both `Runtime::recall` and `HybridRuntime::recall` touch returned hits before
  rendering framing/data. Ordinary `search(...)` remains read-only. This gives
  decay policy a real "last used" signal. Focused result: `test-memory`
  **36 cases / 797 assertions**.

Slice 180 wires long-term memory read-after
  observability through the same configured-route bootstrap boundary as recall
  execution. `oran-hook` now carries typed `MemoryReadPayload` /
  `MemoryReadHitPayload` shapes for successful prompt-boundary long-term recall
  and `memory.recall` tool reads. Default sinks keep source, scope, kind,
  limit, match count, score, and byte/count metadata but do not receive raw
  recall query text or hit title/body/tags/linked ids; `trusted_local` sinks
  receive the raw query and records. `memory_read_after` remains advisory in
  this slice; the blocking `memory_read_before` rewrite/veto consumer stays
  deferred. Focused results: `test-hook` **36 cases / 287 assertions** and
  `test-bootstrap` **116 cases / 969 assertions**.

Slice 179 wires long-term memory lifecycle hooks
  through the configured-route bootstrap memory-tool callbacks. `oran-hook` now
  carries typed `MemoryWritePayload` / `MemoryForgetPayload` shapes plus a
  redacted record summary: default sinks keep id/scope/kind/size/count metadata
  but do not receive memory title/body/tags/linked ids, while
  `trusted_local` sinks receive the full record. `AgentPromptRunner` publishes
  blocking `memory_write_before` before `memory.remember` mutates the backend,
  rejects veto/rewrite/require_approval decisions as
  `ErrorKind::permission_denied` with `reason=blocked_by_hook`, skips the
  backend/vector write on veto, and publishes advisory `memory_write_after` /
  `memory_forget` after successful write/delete paths. Focused results:
  `test-hook` **35 cases / 264 assertions** and `test-bootstrap`
  **116 cases / 929 assertions**.

Slice 178 wires the long-term hybrid-search
  policy into configured-route bootstrap when the binary is built with
  `--vector_memory=y`. `RuntimeAssembly` now owns a separate
  `<workspace>/.orangutan/memory-vectors.db` sqlite-vec pool/backend plus a
  `memory::longterm::HybridRuntime`; `AgentPromptRunner` uses a deterministic
  local text embedding owner (`oran-local-text-v1`, 64 dimensions) to route
  prompt-boundary recall and `memory.recall` through hybrid search, and mirrors
  `memory.remember` / `memory.forget` writes into the vector index. Default
  builds keep the optional dependency off and still reject
  `memory.longterm.hybrid_search.enabled=true` with
  `reason=build_option_disabled`, `option=vector_memory` before assembly or
  provider side effects. Focused results: default `test-memory` **34 cases / 771
  assertions**, default `test-bootstrap` **115 cases / 885 assertions**, gated
  `--vector_memory=y` `test-memory` **36 cases / 791 assertions**, and gated
  `--vector_memory=y` `test-bootstrap` **120 cases / 958 assertions**.

Slice 177 extends the shared 10k-record
  FTS5-vs-vector-vs-hybrid memory benchmark with gated sqlite-vec rows.
  Default `bench-memory` builds still emit only the FTS5-only, brute-force
  vector, and FTS5+brute-force hybrid comparison rows. When configured with
  `--vector_memory=y`, `bench/memory/scenarios/search_fts5_vs_vector.cpp` also
  seeds the shipped `SqliteVecBackend` with the same deterministic keys and
  embeddings and reports sqlite-vec vector-only plus FTS5+sqlite-vec hybrid
  search at `limit=10`. Local gated release result:
  sqlite-vec vector-only **~3.03 ms / batch** and FTS5+sqlite-vec hybrid
  **~18.96 ms / batch**.

Slice 176 adds the optional sqlite-vec long-term
  vector backend behind `--vector_memory=y`. `memory::longterm::SqliteVecBackend`
  now implements the shipped `VectorBackend` contract, opens `sqlite-vec` through
  `storage::Connection` / `Pool` auto-extension registration, migrates a scoped
  `vec0` table with cosine distance, validates configured dimensions, and keeps
  default builds dependency-free by returning a config error from vector
  migration/operations. Focused results: default `test-memory` **32 cases / 760
  assertions**, default `test-storage` **77 cases / 988 assertions**, and gated
  `--vector_memory=y` `test-memory` **34 cases / 780 assertions**.

Slice 175 first consumed the config-side
  `memory.longterm.hybrid_search.enabled` flag at configured-route bootstrap by
  failing fast before vector ownership existed. Slice 178 narrows that guard to
  default builds: `enabled=true` now rejects with `ErrorKind::config`,
  `path=$.memory.longterm.hybrid_search.enabled`,
  `reason=build_option_disabled`, and `option=vector_memory` before opening
  runtime assembly state or contacting a provider. Builds configured with
  `--vector_memory=y` now consume the policy through the assembly-owned vector
  backend and hybrid runtime. Disabled hybrid-search config remains accepted.
  Focused result for the original guard: `test-bootstrap` **115 cases / 883
  assertions**.

Slice 174 adds the config-side hybrid-search
  policy contract for long-term memory. `oran-config` now parses
  `memory.longterm.hybrid_search.enabled`, positive `lexical_limit`,
  `vector_limit`, and `result_limit`, and non-negative finite
  `lexical_weight` / `vector_weight`, rejecting the all-zero weight case so the
  policy matches `HybridRuntime` validation before bootstrap consumes it. The
  block defaults disabled; since slice 178, gated vector builds consume it for
  hybrid recall while default builds still reject enabled hybrid search. Focused
  result: `test-config` **46 cases / 402 assertions**.

Slice 173 adds the first long-term search comparison bench.
  `bench/memory/scenarios/search_fts5_vs_vector.cpp` seeds one shared 10k-record
  corpus and times three searches at `limit=10`: FTS5-only
  `memory::longterm::Runtime::search`, an in-bench brute-force cosine
  `VectorBackend`, and the slice-172 `memory::longterm::HybridRuntime::search`
  over both. The cosine backend is a reference implementation (deterministic
  seeded embeddings, scope-filtered linear scan, `partial_sort` top-K) until a
  gated `--vector_memory=y` sqlite-vec adapter satisfies the same `VectorBackend`.
  Local result (WSL2, release): FTS5-only **~15.42 ms/batch**, vector cosine
  **~675.6 µs/batch**, hybrid **~16.18 ms/batch** — at 10k records the lexical
  scan dominates, so hybrid ≈ FTS5 + ~5%. Bench-only; `test-memory` unchanged at
  **31 cases / 756 assertions**.

Slice 172 adds the first long-term hybrid search composition contract.
  `<oran/memory/longterm.hpp>` exposes `HybridSearchRequest` plus `HybridRuntime`,
  which validates one lexical `Query`, one `VectorEmbedding`, per-backend limits, a
  result limit, and non-negative weights; searches a lexical `Backend` and
  `VectorBackend`; hydrates vector-only keys through `Backend::get`; ignores stale
  vector rows whose records are missing; and returns deterministic weighted
  `SearchHit` rows with populated lexical/vector score components.
  `HybridRuntime::recall` reuses the existing deterministic recall framing
  renderer. Focused result: `test-memory` **31 cases / 756 assertions**.

Slice 171 adds the first 10k-record long-term memory search baseline.
  `bench-memory` now seeds a synthetic FTS5 corpus once and measures
  `memory::longterm::Runtime::search("react agent loop", limit=10)` through the
  default `Fts5Backend`, with scenario-local nanobench settings so the
  expensive search path stays practical to run. Local focused result:
  `memory.longterm_fts5_search_10k_limit10` **~15.08 ms / batch** after
  `xmake build bench-memory && xmake run bench-memory`; the session memory
  comparison in the same run reported **~887.2 us** raw repository append/load
  vs. **~1041.3 us** typed store append/load.

Slice 170 adds the delete-side long-term memory
  tool. `oran-tool` registers deferred `memory.forget` with `write_memory`
  capability, parses `{id}`, and delegates through
  `DispatchContext::memory_forget` so the tool library stays independent of
  `oran-memory`. `oran-memory` now renders structured `memory_forget`
  `data_json` for the scoped removed key, and `AgentPromptRunner` binds the
  callback to the assembly-owned `memory::longterm::Backend`, deriving the
  stable scope key before calling the backend's idempotent `remove(...)` through
  the normal permission/audit/hook/output-cap path. Focused results:
  `test-tool` **208 cases / 2181 assertions**, `test-memory`
  **28 cases / 727 assertions**, and `test-bootstrap`
  **114 cases / 874 assertions**.

Slice 169 adds the first write-side long-term
  memory tool. `oran-tool` registers deferred `memory.remember` with
  `write_memory` capability, parses record fields
  `{id, kind, title, body, importance?, tags?, linked_record_ids?, shadow?}`,
  and delegates through `DispatchContext::memory_remember` so the tool library
  stays independent of `oran-memory`. `oran-memory` now renders structured
  `memory_remember` `data_json` for saved records, and `AgentPromptRunner`
  binds the callback to the assembly-owned `memory::longterm::Backend`, stamping
  the stable scope key plus dispatch-time timestamps before upserting through
  the normal permission/audit/hook/output-cap path. Focused results:
  `test-tool` **204 cases / 2140 assertions**, `test-memory`
  **27 cases / 723 assertions**, and `test-bootstrap`
  **113 cases / 859 assertions**.

Slice 168 adds the first long-term memory tool.
  `oran-tool` registers deferred `memory.recall` with `read_memory` capability,
  parses `{"query", "limit"?, "kinds"?}`, and delegates through a new
  `DispatchContext::memory_recall` callback so the tool library stays
  independent of `oran-memory`. `oran-memory` now renders structured
  `memory_recall` `data_json` from recall hits, and `AgentPromptRunner` binds
  the callback to the assembly-owned `memory::longterm::Runtime`, returning
  deterministic recall text plus record metadata through the normal
  permission/audit/hook/output-cap path. Focused results: `test-tool`
  **200 cases / 2080 assertions**, `test-memory` **26 cases / 714 assertions**,
  and `test-bootstrap` **112 cases / 838 assertions**.

Slice 167 adds the first configured long-term
  recall query-derivation selector. `oran-config` parses
  `memory.longterm.recall.query_strategy` as `prompt_text` (default/current
  behavior) or `last_user_message`; `bootstrap::run` maps that policy into
  `AgentPromptRunnerOptions::longterm_recall`; and `AgentPromptRunner` uses the
  latest previous user text for the single prompt-boundary recall query when
  that strategy is selected, falling back to the current prompt when no prior
  user text exists. Kind filters, limits, disabled-by-default recall, and
  deterministic section-5 rendering remain unchanged. Focused results:
  `test-config` **44 cases / 369 assertions** and `test-bootstrap`
  **111 cases / 824 assertions**.

Slice 166 adds optional long-term recall kind
  filters to the configured-route policy: `oran-config` parses
  `memory.longterm.recall.kinds` as a non-empty unique array of `RecordKind`
  spellings, `bootstrap::run` validates those spellings against
  `memory::longterm::RecordKind`, and `AgentPromptRunner` maps the parsed filter
  into `memory::longterm::Query::kinds` before the same once-per-prompt recall.
  Omitting `kinds` preserves all-kind non-shadow recall, and defaults still keep
  recall disabled. Focused results: `test-config`
  **43 cases / 358 assertions** and `test-bootstrap`
  **110 cases / 811 assertions**.

Slice 165 gives ordinary configured-route startup
  a typed long-term recall policy: `oran-config` parses
  `memory.longterm.recall.enabled` plus positive `limit`, warns or fails on
  unknown nested memory fields through the existing strict-config path, and
  `bootstrap::run` maps that policy into
  `AgentPromptRunnerOptions::longterm_recall` after building the provider-backed
  runtime assembly. Defaults keep recall disabled and the binary still uses the
  runner's prompt+scope query derivation when enabled. Focused results:
  `test-config` **43 cases / 348 assertions** and `test-bootstrap`
  **108 cases / 795 assertions**.

Slice 164 wires opt-in long-term recall into
  `AgentPromptRunner`: `AgentPromptRunnerOptions::longterm_recall` defaults
  disabled, requires a positive limit plus an assembly-owned
  `memory::longterm::Runtime`, rejects exact `memory_framing` overrides, derives
  a prompt-boundary query from the current user prompt and stable scope key, and
  renders section 5 once from returned record-only `memory::Framing` bytes before
  `agent::Loop`. Focused result: `test-bootstrap` **107 cases / 784 assertions**.

Slice 163 wires long-term memory into
  `RuntimeAssembly`: configured-route startup now opens and migrates a separate
  `<workspace>/.orangutan/memory.db`, owns `memory::longterm::Fts5Backend` plus
  `memory::longterm::Runtime`, exposes `longterm_memory_backend()` /
  `longterm_memory_runtime()` / path/status accessors, and keeps built-in
  no-provider startup disabled so fresh CLI runs do not create memory state.
  Focused result: `test-bootstrap` **104 cases / 756 assertions**.

Slice 162 lands the first long-term memory
  runtime composition layer: `memory::longterm::Runtime` wraps a lexical
  `Backend`, validates search/recall requests before dispatch, returns
  `RecallResult { hits, framing }`, and renders deterministic section-5
  `memory::Framing` bytes from recalled records via `render_recall_framing(...)`.
  Focused result: `test-memory` **25 cases / 705 assertions**.

Slice 161 landed the default long-term memory
  lexical backend: `memory::longterm::Fts5Backend` owns the SQLite FTS5 schema
  under `src/oran-memory/migrations/longterm/`, applies its embedded migration
  through `storage::Pool`, and implements scoped `get` / `search` / `upsert` /
  idempotent `remove` with kind/shadow filters and lexical scores. The xmake
  SQLite package is now built with `SQLITE_ENABLE_FTS5`. Focused result:
  `test-memory` **20 cases / 678 assertions**.

Slice 160 landed the first long-term memory backend-contract prework from the
  deep-review tracker: `<oran/memory.hpp>` exports
  `memory::longterm::RecordKind`, `RecordKey`, `Record`, `Query`, `SearchHit`,
  `WriteRequest`, the lexical `Backend`, the vector `VectorBackend`, vector
  request/result shapes, and validation helpers for record/search/vector
  inputs. This pinned the seam the FTS5 and gated sqlite-vec implementations
  must satisfy without runtime wiring yet. Focused result: `test-memory`
  **16 cases / 623 assertions**.

Slice 159 closes the P3 async `Runtime::Impl::run()` clarification item from
  the deep-review tracker: the private runtime lifecycle is now a single
  `idle/running/stopped` state, `Runtime::run()` explicitly rejects reuse after
  stop with `ErrorKind::conflict`, and exceptions escaping `io_context.run()`
  handlers are contained inside the IO worker, stop the runtime, and return from
  `run()` as a structured `ErrorKind::internal` failure instead of escaping a
  worker thread. Focused result: `test-async` **11 cases / 51 assertions**.

Slice 158 closes the P3 hook multi-sink payload
  sharing item from the deep-review tracker: `hook::PayloadPtr` is now the
  shared immutable sink-delivery handle, `hook::Bus` builds at most one raw
  payload snapshot and one default/redacted snapshot per advisory or blocking
  publish, and default sinks share the redacted view while `trusted_local`
  sinks share the raw view. This preserves the slice-152 trust boundary without
  cloning large structured hook bytes once per sink. Focused results:
  `test-hook` **34 cases / 243 assertions** and `bench-hook`
  (`publish_no_sinks` ~309 ns, `publish_one_sink` ~1.57 µs,
  `publish_three_sinks` ~3.77 µs, large redacted 1/3 default sinks
  ~4.05 µs / ~6.58 µs on this local run). Slice 157 closes the P3 storage
  `Pool` mutex-contention measurement item from the deep-review tracker:
  `bench-storage` now includes an acquire-only reader-pool pair that compares
  32 sequential uncontended `Pool::acquire_reader` leases with a single-slot
  FIFO waiter-drain batch where every waiter queues behind a held lease. Local
  result: `storage.pool_reader_uncontended_acquire_batch` ~6.71 us and
  `storage.pool_reader_contended_fifo_acquire_batch` ~18.96 us per 32-acquire
  batch on a noisy local nanobench run. Slice 156 closes the parallel advisory
  hook fan-out
  cleanup item from the deep-review tracker: `hook::Bus::publish_advisory`
  now starts every subscribed sink as a sibling child coroutine, gathers
  completions through the existing bounded `async::Channel`, preserves
  subscription-ordered `PublishOutcome` rows, keeps per-sink redaction before
  delivery, and propagates parent cancellation to children before draining
  completion rows. Blocking hook publishes intentionally remain sequential
  because their first non-`proceed` decision short-circuits. Focused results:
  `test-hook` **33 cases / 231 assertions** and `bench-hook`
  (`publish_no_sinks` ~325 ns, `publish_one_sink` ~1.63 µs,
  `publish_three_sinks` ~4.07 µs on this local run). Slice 155 closes the public
  `tool::DispatchContext::for_now()` cleanup item from the deep-review tracker:
  `DispatchContext::for_now(executor, rules, audit, scope_key, agent_key,
  identity)` creates a fresh current-clock context, and
  `DispatchContext::for_now(prototype, thread_approval_token_output)` clones
  long-lived dispatch services while refreshing `now` and clearing
  dispatch-local `registry` / `resolved_path` fields. `agent::ToolScheduler`
  now uses the prototype overload for per-call contexts, and
  `bootstrap::AgentPromptRunner` uses the fresh overload for configured-route
  dispatch setup; aggregate initialization remains available for fixed-clock
  tests. Focused results: `test-tool` **197 cases / 2049 assertions**,
  `test-agent` **56 / 10 744**, and `test-bootstrap` **101 / 711**. Slice 154 closes the public `io::run_blocking`
  utility item from the deep-review tracker: `<oran/io/blocking.hpp>` now
  exports the same cancel-before/cancel-after-post boundary the file helpers
  used privately, restricted to nullary callables returning `core::Result<T>`.
  `oran-io`'s own file/directory helpers consume that public template, keeping
  future short blocking IO callers on the same cancellation contract instead of
  copying coroutine-posting glue. Focused result: `test-io`
  **54 cases / 311 assertions**. Slice 153 closes the atomic-write durability item from
  the deep-review tracker: `io::WriteTextOptions` now carries
  `WriteTextDurability { rename_only, fsync_file, fsync_file_and_parent }`.
  The default `rename_only` keeps the slice-32 temp-then-rename behavior,
  while the fsync modes sync the staged file and, for
  `fsync_file_and_parent`, the parent directory after a successful rename.
  Non-default durability rejects unless `atomic=true`, atomic temp leaves now
  use `.<name>.orangutan.tmp.<pid>.<random>` with exclusive create/retry
  instead of a process-local counter, and `oran-io` invalidates file-view
  caches immediately after the rename even if a later parent fsync reports
  failure. Focused result: `test-io` **52 cases / 303 assertions**. Slice 152
  closes the sensitive mutation-input hook
  redaction item from the deep-review tracker: tool and approval hook payloads
  that carry `input_json` now also carry optional `redacted_input_json`, and
  `hook::Bus` substitutes that sanitized view for every sink whose kind is not
  `SinkKind::trusted_local` across both advisory and blocking publishes.
  `Registry::dispatch` fills the sanitized view for `file.write` and
  `file.edit` with a deterministic `redacted_tool_input` JSON object carrying
  the full input SHA-256, input byte count, redacted field names, and string
  byte counts; malformed JSON still receives a hash-only summary. Trusted-local
  sinks continue to receive the original mutation input. Focused result:
  `test-hook` **32 cases / 222 assertions** and `test-tool`
  **195 cases / 2031 assertions**. Slice 151 closes the nested config strictness gap
  from the deep-review tracker: loose config loads now emit `ConfigWarning`
  rows for unknown fields inside typed provider profiles, provider pricing,
  routes, and hooks, while `strict_config=true` or
  `LoadOptions::strict_unknown_fields=true` turns those same fields into
  `ErrorKind::config` failures. Reserved but untyped `hooks.sinks` /
  `hooks.bindings` still remain accepted placeholders until external hook sink
  models land. Focused result: `test-config` **40 cases / 322 assertions**.
  Slice 150 consumes the already parsed
  `trace.retention_days` policy at bootstrap/runtime assembly time:
  `storage::TraceRepository` now exposes
  `purge_turns_started_before(started_before_ns)`, which deletes only
  `trace_turns` rows older than an explicit Unix-nanosecond cutoff and leaves
  `audit_events` untouched, and `bootstrap::run` derives that cutoff from the
  configured retention window before `RuntimeAssembly::build` exposes the
  long-lived trace repository. This closes the trace-retention runtime wiring
  gap while keeping storage clock-free and audit retention separate. Focused
  result: `test-storage` **76 cases / 986 assertions** and `test-bootstrap`
  **101 / 711**. Slice 149 promotes the transcript
  activation/deactivation scan to the `oran-skill` public API. The new
  `skill::SkillActivationEvent` and
  `skill::skill_activation_events_from_transcript(...)` primitive returns
  ordered semantic events from successful `skill.invoke` / `skill.deactivate`
  tool results, optionally starting at a transcript suffix boundary.
  `skill::active_skills_from_transcript(...)` now nets those events, and
  `AgentPromptRunner` maps the same shared event stream into
  `memory::session::SkillActivationUpdate` rows for session persistence instead
  of owning a second skill-result parser. This keeps section-4 rendering and
  `oran-prompt` unchanged while future CLI/web/channel/automation runtime
  owners can persist or replay the same events. Focused result: `test-skill`
  **27 cases / 168 assertions** and `test-bootstrap` **100 / 699**. Slice 148
  makes skill activation state durable in session memory: `sessions.db` adds
  `session_skill_activations` keyed by `(session_id, agent_key, skill_name)`,
  `storage::SessionRepository` and `memory::session::Store` expose typed
  upsert/load APIs, and
  `skill::ActivationPolicy::session_skill_activations` overlays the
  transcript-derived `skill.invoke` / `skill.deactivate` state before config
  deactivation and expiration subtract from it. `AgentPromptRunner` loads those
  durable rows before section-4 rendering and records successful activation /
  deactivation updates after successful transcript persistence, so transcript
  compaction or pruning cannot resurrect a skill whose latest durable state is
  inactive. The old transcript scan remains the fallback and compatibility path
  when session memory is disabled. Focused result: `test-skill`
  **26 cases / 163 assertions**, `test-storage` **75 / 968**, `test-memory`
  **9 / 576**, and `test-bootstrap` **100 / 699**. Slice 147 adds the
  permissioned `skill.deactivate` built-in (capability `deactivate_skill`): a
  successful call records a versioned `skill_deactivation` result in the session
  transcript, `skill::active_skills_from_transcript` nets `skill.invoke`
  activations against `skill.deactivate` deactivations in transcript order (most
  recent event wins), bootstrap installs the `DispatchContext::skill_deactivate`
  callback, and the tool joins the default active catalog. Transcripts without a
  deactivation behave exactly as before. Focused result: `test-skill`
  **24 cases / 155 assertions**, `test-tool` **191 / 1919**, `test-core`
  **71 / 459**, and `test-bootstrap` **99 / 667**. Slice 146 adds the first
  runtime-owned source for the slice-143/144/145 `skill::ActivationPolicy`
  inputs: `oran-config` parses optional `agents.<name>.skills_deactivated`
  (unique non-empty names) and `agents.<name>.skills_expirations`
  (`{name, expires_at}` rows with strict UTC ISO-8601 `expires_at`), and
  `AgentPromptRunner` builds a non-empty `skill::ActivationPolicy` from the
  selected agent config — mapping `config::SkillExpirationConfig` to
  `skill::SkillExpiration` and supplying
  `evaluation_time = core::time::now_utc()` at the prompt boundary only when
  expirations are present — so configured-route section 4 now drops deactivated
  and expired active markers while the renderer stays clock-free.
  Default/no-agent callers still pass an empty policy, so unchanged configs
  behave exactly as before. Focused result: `test-config` **39 cases / 299
  assertions** and `test-bootstrap` **98 cases / 646 assertions**. The broader
  product path remains cross-runtime skill policy ownership without changing
  the prompt-builder boundary. Slice 145 extends
  `skill::ActivationPolicy` with explicit `SkillExpiration` rows plus optional
  `evaluation_time`, validates expiration names as unique single-line skill
  names, requires that evaluation time when expirations are supplied, and has
  `skill::resolve_active_skills(...)` subtract expired names from
  transcript-derived active markers after loaded/allowed catalog filtering. The
  default bootstrap runner still supplies no expiration rows or evaluation time,
  so configured-route behavior is unchanged until a future runtime source
  provides explicit expiration state. Focused result: `test-skill`
  **21 cases / 133 assertions**. Slice 144 extends `skill::ActivationPolicy` with
  `deactivated_skill_names`, validates that policy input as unique single-line
  skill names, and has `skill::resolve_active_skills(...)` subtract those names
  from transcript-derived active markers after loaded/allowed catalog
  filtering. The default bootstrap runner still supplies an empty deactivation
  set, so configured-route behavior is unchanged until a future runtime source
  provides explicit deactivation events. Focused result: `test-skill`
  **19 cases / 121 assertions**. The previous doc-only slice specified the
  section-4 cache semantics future policy extensions must preserve. Slice 143 adds
  `skill::ActivationPolicy` plus `skill::resolve_active_skills(...)`, making
  the current transcript-derived active-marker policy an `oran-skill` public
  concept before future expiration/deactivation rules land. `AgentPromptRunner`
  now asks that policy resolver to filter activation markers against the
  current loaded/allowed skill catalog before rendering section 4, while the
  active turn still receives skill bodies only as conversation-tail tool-result
  text. Focused result: `test-skill` **17 cases / 111 assertions** and
  `test-bootstrap` **95 cases / 617 assertions**. Slice 142 adds
  `skill::ActiveSkill` metadata plus versioned `skill.invoke` activation
  `data_json`, has `oran-skill` derive active markers from successful
  session transcript tool results, and has `AgentPromptRunner` render
  deterministic `Active Skill: <name>` rows in section 4 before the next
  prompt when the skill is still loaded and allowed. The invoked skill body
  still arrives only as conversation-tail tool-result text, so the active
  turn's cached prefix stays unchanged. Focused result: `test-skill`
  **14 cases / 102 assertions** and `test-bootstrap`
  **95 cases / 617 assertions** (rerun through the localhost-safe path after
  the expected sandbox `open: Operation not permitted` false negative). Slice
  141 adds
  `agents.<name>.prompt_overlay` to `oran-config` and has
  `AgentPromptRunner` copy the selected agent's stable section-6 prompt text
  into `RunTurnInputs::per_agent_overlay` when callers did not supply exact
  overlay bytes. The same selected `agents.<name>` lookup now feeds both
  `prompt_overlay` and `skills_enabled`, while permission materialization keeps
  using the existing `permission_agent_name` path. Focused result:
  `test-config` **37 cases / 278 assertions** and `test-bootstrap`
  **94 cases / 597 assertions**. Slice
  140 wires the existing `--mode` / `--agent`
  bootstrap selectors into configured-route `bootstrap::run`: `--mode` selects
  the permission baseline passed to `AgentPromptRunner`, `--agent <name>`
  selects the runtime `agents.<name>` permission overlay, skill allowlist, and
  session/audit agent key, and the no-provider deterministic shell rejects
  selector flags unless `--explain-rules` is active. The diagnostic
  `--explain-rules` path keeps the same selector semantics. Focused result:
  `test-bootstrap` **93 cases / 586 assertions**. Slice 139 adds optional
  `agents.<name>.skills_enabled` parsing to `oran-config` and lets
  `AgentPromptRunner` select an `agents.<name>` entry through
  `AgentPromptRunnerOptions::agent_config_name` (falling back to
  `permission_agent_name` for current selected-agent callers). The runner
  filters the workspace skill snapshot through that allowlist before replacing
  section 4 and before serving `skill.invoke`; absent allowlists keep all
  loaded skills visible, present empty arrays enable none, and filtered-out
  skill names use the existing model-repairable `skill_not_loaded` path.
  Focused result: `test-config` **37 cases / 274 assertions** and `test-bootstrap`
  **90 cases / 566 assertions**. Slice 138 adds `skill::WorkspaceSkillSnapshot`,
  a prompt-boundary workspace skill refresh owner that watches
  `<workspace>/.orangutan/skills` with Linux inotify when available, keeps a
  bounded content-aware directory signature so unchanged prompts avoid reloads,
  invalidates `oran-io` file-view cache entries before re-reading changed skill
  markdown, and has `AgentPromptRunner` refresh the section-4 catalog plus the
  `skill.invoke` document snapshot before each prompt when callers supplied a
  `skills_directory`. Exact `skills_catalog` bytes still bypass directory
  loading for tests/embedders, missing directories remain empty snapshots, and
  each active turn sees one coherent catalog/body snapshot. Focused result:
  `test-skill` **11 cases / 89 assertions** and `test-bootstrap`
  **88 cases / 539 assertions**. Slice 137 adds the permissioned
  `skill.invoke` built-in to the default active catalog and has
  `AgentPromptRunner` serve it from the immutable workspace skill snapshot
  loaded before loop entry. The built-in parses only `{"name": <string>,
  "inputs"?: <json>}`, delegates execution through `DispatchContext`, and
  returns the skill body as ordinary model-visible tool-result text for the
  next provider iteration; missing runtime services or unknown loaded skills
  remain model-repairable tool errors. Skill bodies still stay out of section 1
  and section 4. Focused result: `test-tool` **188 cases / 1893 assertions**,
  `test-bootstrap` **87 cases / 518 assertions** (rerun outside the localhost
  socket sandbox after the expected `open: Operation not permitted` false
  negative), `test-prompt` **10 / 98**, and `test-agent`
  **56 / 10 744**. Slice 136 adds
  `skill::Loader`, a bounded markdown/frontmatter snapshot reader for
  `<workspace>/.orangutan/skills/*.md`, and has configured-route
  `AgentPromptRunner` load the workspace skills directory once before the first
  prompt unless callers supplied exact `skills_catalog` bytes. Missing skills
  directories produce an empty catalog, malformed frontmatter or oversized
  bodies fail before the loop, and the body text stays out of section 4.
  Focused result: `test-skill` **9 cases / 55 assertions** and
  `test-bootstrap` **86 cases / 504 assertions**. Slice 135 adds `oran-skill`
  with a deterministic section-4 catalog renderer
  plus `skill::CatalogOwner`; `AgentPromptRunner` now renders that catalog once
  before loop entry, and bootstrap passes the pre-rendered section through the
  existing prompt builder boundary. Focused result: `test-skill` **5 cases / 19
  assertions**, `bench-skill` the deterministic renderer vs. a local
  order-trusting baseline, `test-bootstrap` **85 cases / 491 assertions**.
  Slice 134 adds `agent::SystemPreamble` / `agent::SystemPreambleOwner` and the
  repository default section-1 preamble. `agent::Loop` uses its owned default
  when callers leave `RunTurnInputs::system_preamble` empty, and
  `AgentPromptRunner` renders the stable preamble once before loop entry. The
  prompt-cache fixture now uses the repository default preamble, and
  `scripts/check-prompt-preamble.sh` runs from `scripts/ci.sh` to reject clocks,
  ids, and cross-section prompt bytes in the default preamble. Focused result:
  `test-agent` **56 cases / 10 744 assertions** and `test-bootstrap` **84 cases
  / 477 assertions**.
  Slice 133 finished the memory runtime v1 plan by adding
  `memory::FramingOwner` as the minimal once-per-turn section-5 owner. It keeps
  `prompt::Builder` unchanged, and slice 164 now uses the same `memory::Framing`
  value from an opt-in `AgentPromptRunnerOptions::longterm_recall` path at the
  prompt boundary before `agent::Loop` starts, even when the provider/tool path
  needs multiple iterations. Focused result: `test-memory` **8 cases / 559
  assertions** and `test-bootstrap` **83 cases / 464 assertions**.
  Slice 132 landed `AgentPromptRunner` load/append persistence through
  `RuntimeAssembly::session_store()` before each prompt and kept the previous
  in-process transcript behavior as the fallback when session memory is
  disabled. Focused result: `test-bootstrap` **82 cases / 452 assertions**.
  Slice 131 extends
  `RuntimeAssembly` with `sessions_db_path`, session enablement, sessions pool
  sizing/cache options, `session_store()`, and `sessions_path()`. The assembly
  runs the session migration, opens `<workspace>/.orangutan/sessions.db` by
  default, and exposes a typed `memory::session::Store` over a separate
  `storage::SessionRepository`; configured-route `bootstrap::run` enables that
  owner while the built-in no-route CLI path explicitly disables it. The startup
  banner now reports `sessions=enabled|disabled` and the sessions path. Focused
  result: `test-bootstrap` **80 cases / 422 assertions**. Slice 130 adds the
  first `oran-memory` implementation target: `<oran/memory.hpp>` exports
  `memory::session::Store`, which wraps `storage::SessionRepository` with typed
  `SessionId` / `AgentKey` / `SessionSummary` values and private
  `core::Message` JSON serialization for text, thinking, tool-use, and
  tool-result content blocks. Storage remains JSON-opaque and role-typed. New
  `test-memory` coverage pins ordered round-trips, agent scoping, malformed
  stored-row rejection, id validation, and the spec's 500-message round-trip
  criterion; `bench-memory` compares raw `SessionRepository` append/load with the
  typed memory wrapper. Focused result: `test-memory` **5 cases / 550
  assertions**; `bench-memory` ran `memory.session_repository_append_load`
  **~849.8 us / batch** vs. `memory.session_store_append_load` **~1022.4 us /
  batch**. Slice 129 adds provider profile-priced cost calculation:
  `config::ProfileConfig`
  now carries optional `pricing` fields for per-million input, output, cache
  creation, and cache read tokens; route resolution preserves those values on the
  loop-facing `provider::ModelTarget`; and `agent::Loop` now fills
  `provider::Usage::cost_estimate` from the selected target when a provider
  response leaves it unset. Existing provider-supplied costs still win. Focused
  result: `test-config` **37 cases / 270 assertions**, `test-provider`
  **86 / 652**, and `test-agent` **52 / 10 705**. Slice 128 adds slash-command
  handling to the CLI REPL paths: `/help` prints a short command list,
  `/exit` and `/quit` stop the REPL without dispatching to the prompt runner,
  and both scripted and interactive REPL input trim leading and trailing ASCII
  whitespace before command recognition. `run` and `run_async` keep non-command
  prompts flowing to the existing runner or deterministic shell paths. Focused
  result: `test-cli` **26 / 205** (+3 cases, +29 assertions). Slice 127 adds
  the first provider usage rollup read over trace storage: `TraceRepository::list_provider_usage_rollups`
  groups existing `trace_turns` usage by UTC day, agent key, route profile, and
  route model, sums input/output/cache tokens plus already-recorded
  `cost_estimate_usd`, and supports optional agent/profile/model filters. This is
  trace-derived aggregation only; slice 129 then adds profile-priced cost
  calculation on top of those recorded rows. Focused result: `test-storage`
  **73 / 938** (+1 case, +39 assertions). Slice 126 publishes the first
  provider lifecycle hooks from
  `agent::Loop`.
  `RunTurnInputs` now accepts the process `hook::Bus` plus
  scope/agent/identity/origin metadata, and the loop emits advisory
  `provider_request`, `provider_response`, `provider_error`, and
  `provider_fallback` payloads around each provider await. Payloads carry
  route/profile/model/protocol, counts, retry knobs, usage, stop/error, and
  timing metadata only — no prompt text, messages, headers, credentials, or raw
  provider bodies — so `oran-provider` stays hook-free while `oran-agent` owns
  the lifecycle observation point. `AgentPromptRunner` wires
  `RuntimeAssembly::hook_bus()` into configured-route turns. Focused result:
  `test-hook` **30 / 207**, `test-agent` **50 / 10 689** (+3 cases, +71
  assertions), and `test-bootstrap` **77 / 380** (+1 case, +25 assertions).
  Slice 125 replaces the configured-route placeholder REPL with a
  provider-backed terminal input loop: `bootstrap::run` now enables
  `CliOptions::interactive_repl` when it calls `cli::run_async` with
  `AgentPromptRunner`, and `run_async` reads terminal stdin through a persistent
  asio descriptor/buffer until an empty line or EOF. Each non-empty interactive
  line is dispatched to the runner as `CliMode::repl` with increasing
  `prompt_index`; scripted `repl_lines` still win for tests/noninteractive
  drivers, and no-runner/built-in default shells remain deterministic and
  nonblocking. Focused result: `test-cli` **23 / 176** (+5 cases, +66
  assertions) and `test-bootstrap` **76 / 355** (+1 case, +11 assertions).
  Slice 124 adds
  the provider-side OpenAI Responses SSE decoder:
  `OpenAiResponsesSseDecoder` lives beside `AnthropicSseDecoder` under
  `src/oran-provider/_impl/`, consumes OpenAI Responses stream events
  (`response.output_text.delta`, reasoning deltas,
  `response.function_call_arguments.*`, terminal `response.completed` /
  `response.incomplete` / `response.failed`, and `error`), drives the existing
  `provider::EventSink` callbacks for live output, and decodes the terminal
  embedded `response` through `decode_protocol_response` so streaming and body
  paths assemble the same `provider::Response`. `ProtocolTransportSystem` now
  streams both Anthropic Messages and OpenAI Responses when `request.stream` is
  set and the injected transport reports `supports_streaming() == true`;
  otherwise it keeps the existing body path. Focused result: `test-provider`
  81 / 614 → **86 / 643** (+5 cases, +29 assertions); other buckets were not
  run for this narrow provider slice. Slice 123 (C) just closed the
  provider-SSE-streaming arc: streaming now runs end-to-end into the binary.
  Bootstrap's `HttpProtocolTransport` overrides `supports_streaming()` → true and
  implements `send_streaming` over `http::Client::send_streaming` (translating
  each `http::SseEvent` into the provider `ProtocolSseCallback`);
  `cli::StreamingPromptSink` (a `provider::EventSink`) renders answer/thinking
  deltas to an injectable `std::ostream` (default `std::cout`, flushed per delta)
  plus a one-line `[tool: <name>]` marker; and `AgentPromptRunner` builds the sink
  for non-quiet streaming runs, passes it to `agent::Loop::run_turn`, and clears
  the assembled `PromptRunResult::text` once it streamed live so the CLI does not
  double-print. Configured-route `orangutan --prompt` over Anthropic now renders
  tokens character-by-character (spec 0001 AC3); a mid-stream Ctrl-C surfaces as
  `Error::cancelled` with `cancellation_phase=provider` (spec 0018). `oran-cli`
  now depends on `oran-provider` (downward, interface → composition). `test-cli`
  14 / 97 → **18 / 110**; `test-bootstrap` 72 / 316 → **75 / 344**. Slice
  122 (B) landed the provider-side Anthropic Messages streaming: the
  stateful `AnthropicSseDecoder` (`src/oran-provider/_impl/`, the incremental
  sibling of `decode_protocol_response` that assembles a byte-identical
  `Response`), a provider-owned
  `ProtocolTransport::send_streaming(ProtocolHttpRequest, ProtocolSseCallback)`
  seam guarded by a `ProtocolTransport::supports_streaming()` capability query
  (default false), and the Anthropic `ProtocolTransportSystem` honoring
  `request.stream` — when the caller asks to stream and the transport advertises
  support it sends `stream=true` and drives `send_streaming` + the decoder,
  calling the caller's `EventSink` with ordered `on_text_delta` /
  `on_thinking_delta` / `on_tool_start` / `on_tool_delta` / `on_done`; otherwise
  it keeps the body path. OpenAI Responses stayed body-only during that arc; the
  slice 124 follow-up adds its SSE decoder and lifts the same transport gate for
  OpenAI Responses. The capability gate kept configured-route production
  body-only through slice 122; slice 123 then overrode
  `HttpProtocolTransport::supports_streaming()` / `send_streaming`, so the binary
  now streams and `test-bootstrap`'s localhost round trip asserts `"stream":true`.
  Retry /
  fallback stream-suppression needed no new code: the slice-97
  `execution::Runtime` per-attempt `AttemptSink` already retries a pre-first-byte
  failure and returns `stream_already_emitted` once a delta has fired (both arms
  pinned for streaming). The decoder is its own `_impl` TU (not appended to
  `protocol_response.cpp`), so no `nlohmann` provider TU grows toward the cap and
  `compile_budget.json` is unchanged. `test-provider` 66 / 528 → **81 / 614**;
  `test-bootstrap` and `test-agent` unchanged. Slice 121 (A) just
  landed the self-contained `oran-http` SSE transport: `<oran/http.hpp>` exports
  `http::SseEvent` and `Client::send_streaming(BodyRequest, SseEventCallback)`,
  backed by an incremental `text/event-stream` parser in
  `src/oran-http/_impl/sse_parser.hpp` (handles `\n`/`\r\n`, `field: value`,
  multi-line `data:`, blank-line dispatch, `event` defaulting to `message`,
  comment/`id`/`retry` ignore, chunk-split events). The parser runs inside the
  libcurl write callback on the blocking executor; each complete event is
  `asio::post`-ed to the caller's coroutine executor before the sink fires, so
  the decoder/sink never run on the curl thread. A 2xx `text/event-stream`
  resolves with status + headers and an empty body; any other response is
  collected as a body for the caller to decode; the existing 50 ms curl poll
  surfaces mid-stream cancellation. `oran-http` also gained its first
  `compile_budget.json` category (`{1.0, 2.0, 2.5}`, the `oran-async` tier).
  `test-http` 3 / 21 → 15 / 60. Slice 120 wired `agent::Loop` to dispatch every tool batch (including
  N=1) through `ToolScheduler::run_batch` instead of the sequential
  `for (use : tool_uses) registry.dispatch(...)` loop, so bounded parallelism,
  per-path read/write locks, per-call timeout, and parent-cancellation
  propagation apply on the production ReAct path; ordered batch results convert
  to `tool_result` blocks with the same semantics the sequential path used
  (model-repairable per-call errors → tool_result error blocks; a parent
  cancellation or per-call infrastructure error ends the turn with
  `cancellation_phase=tools`). `bootstrap::AgentPromptRunner` now constructs and
  owns a persistent `ToolScheduler` (runner executor + builtin registry + the
  new `runtime.tool_scheduler.{max_parallel_tools, per_call_timeout_ms,
  idle_lock_ttl_ms}` config block, defaults 4 / 60000 / 300000) and threads it
  into `RunTurnInputs::scheduler`; a caller that omits it gets a per-turn
  fallback with default options, so the batched dispatch path is uniform. A
  single-call batch threads the prototype's `approval_token_output` so blocking
  ask-approval replay still works; a parallel batch drops it (one slot cannot
  disambiguate N issued tokens). The scheduler's semaphore-cancel early-return
  now resets its cancellation filter before sending completion, so a call
  cancelled while queued reports cleanly instead of being mis-named a
  `cancellation_lag` laggard. New benches: `scheduler_overhead` (single-call
  `run_batch` ≈ 6.7 µs vs ≈ 2.4 µs direct dispatch — under spec 0002's ≤ 75 µs
  scheduler allowance, AC12) and `scheduler_audit_fanout` (8-call batch
  ≈ 134 µs via `StorageAuditSink` vs ≈ 44 µs via `NullAuditSink`,
  ≈ 11 µs/audit-row, no writer starvation). Focused result: `test-config`
  33 / 241 → **36 / 258**, `test-agent` 45 / 10 607 → **47 / 10 618**,
  `test-bootstrap` unchanged at 72 / 316. Slice 119 closed AC5: parent
  cancellation now ends every **cancel-aware** in-flight call within a 100 ms
  grace window. `ToolScheduler::run_batch` emits on each child's
  `asio::cancellation_signal`, disables its own cancellation filter
  (`reset_cancellation_state(disable_cancellation())`), then races the
  remaining drain against `async::sleep_for(kCancellationGrace=100 ms)` and
  returns `Error::cancelled` with `reason=parent_cancelled` instead of waiting
  unbounded. A handler that ignores its cancellation slot cannot be forced to
  stop — asio cancellation is cooperative, and `co_await (dispatch || timeout)`
  does not resolve until that handler returns (the `wait_for_one` parallel
  group cancels the loser but still awaits it) — so once the grace window
  expires the scheduler stops awaiting it: it records a `cancellation_lag`
  audit row (`event_kind=cancellation_lag`,
  `metadata_json.error_kind=cancellation_lag`) naming the offending tool and
  returns, leaving the laggard detached but alive via the shared `BatchState`
  (the per-call timeout still backstops its eventual exit). A batch of purely
  cancel-aware tools records no such row. The production change is
  `src/oran-agent/scheduler.cpp` only (the `run_call` race is unchanged); two
  new `tests/agent/test_scheduler.cpp` cases pin selective naming + the prompt
  return and the no-false-positive path. Focused result: `test-agent`
  43 / 10 590 → **45 / 10 607** (+2 cases, +17 assertions); the other 13 test
  suites are unchanged. Slice 118
  just closed most of AC7 as a **verification slice with no production change**:
  three new `tests/agent/test_scheduler.cpp` cases prove that under
  `ToolScheduler` parallelism an N-call batch records exactly N
  permission-decision rows and emits exactly N `tool_after` publishes (failures
  included, regardless of completion order); that each `Verdict::ask` resolves
  on its own held slot, with a denied call surfaced at its own ordered index
  rather than hidden behind a successful one; and that slice-67 same-row usage
  enrichment stays correct for two identical concurrent calls because each
  `update_metadata` consumes exactly one not-yet-enriched row via the
  `previous_metadata_json` match (the two enrichments pair 1:1 with the two
  rows). Tracing the dispatch path showed the existing code already guarantees
  these invariants, so slice 118 pins them with tests instead of minting the
  "stronger per-call correlation" the spec hedged on. Focused result:
  `test-agent` 40 / 10 545 → **43 / 10 590** (+3 cases, +45 assertions); the
  other 13 test suites are unchanged. The earlier maintenance slice did not
  change production behavior; it hardened localhost
  HTTP test fixtures so proxy-heavy developer environments no longer leave
  `xmake test` waiting on a one-shot server thread. Slice 117 lands the
  per-canonical-path read/write lock table behind `agent::ToolScheduler`.
  `<oran/agent/scheduler.hpp>` now exposes `ToolSchedulerLockStats` and
  `ToolScheduler::lock_stats()` / `reap_idle_locks(core::Time)`. The lock
  table itself lives in `src/oran-agent/_impl/path_lock_table.hpp` /
  `src/oran-agent/path_lock_table.cpp` and is single-strand by contract
  (matching `core::BoundedCache`). Tools declaring `Capability::write_file`,
  `edit_file`, or `delete_path` take an exclusive lock; tools declaring
  `read_file` or `list_directory` take a shared lock; tools with no
  filesystem capability (and built-ins like `tool.search`) skip path locking
  and run under bounded parallelism only. The lock key is the
  workspace-resolved absolute path the scheduler derives from
  `prototype.workspace` plus the JSON `path` field, matching the canonical
  path the registry's own pre-resolution stamps on `DispatchContext::resolved_path`
  for the common case. Per-entry state is FIFO: a queued exclusive blocks
  new shared acquirers from skipping the line, but consecutive shared
  waiters fan out together when no writer is active. Cancellation during
  wait is reconciled — if a release has already pre-incremented the
  counter on the cancelled waiter's behalf, the cancel arm undoes that
  increment and forwards the wake to the next waiter so the queue does
  not stall. `ToolSchedulerOptions::idle_lock_ttl` (default 5 min) sweeps
  idle entries on `reap_idle_locks(core::Time)`; the future periodic tick
  hangs off `agent::Loop` or a runtime service in a later slice. Slice
  117's focused result: `test-agent` 32 / 462 →
  **40 / 10 545** (+8 cases, +10 083 assertions); the AC10 test runs
  10 000 acquire/release cycles inside a single Catch2 case, which is the
  dominant assertion source. The other 13 test suites are unchanged.
  Slice 116 opens
  `agent::ToolScheduler` in `oran-agent` (`<oran/agent/scheduler.hpp>`,
  `src/oran-agent/scheduler.cpp`, `tests/agent/test_scheduler.cpp`) with
  channel-as-semaphore bounded parallelism via existing
  `async::Channel<std::monostate>` (one permit per `max_parallel_tools`
  slot), per-call timeout via
  `asio::experimental::awaitable_operators::operator||` against
  `async::sleep_for`, ordered results in original `tool_use` order, and
  parent-cancellation propagation via one
  `asio::cancellation_signal` per spawned call (held in a
  `std::deque<asio::cancellation_signal>` so addresses stay stable across
  emplace because the signal is neither copyable nor movable). The scheduler
  brace-initialises a fresh `tool::DispatchContext` per call from the
  caller-supplied prototype so concurrent dispatches do not race on the
  prototype's `registry` / `resolved_path` / `approval_token_output` / `now`
  fields. The scheduler is unwired from `agent::Loop` until slice 120;
  `tool::Registry` stays single-threaded per the spec, with concurrent
  dispatches safe because `Registry::dispatch` is `const` and the
  registry's `entries_` map is read-only after boot. `ToolSchedulerOptions`
  carries `max_parallel_tools=4`, `per_call_timeout=60 s`, and
  `idle_lock_ttl=300 s` defaults from the spec; the lock-TTL field is
  stored now so slice 117's lock-table consumer does not have to revise
  the option struct. Tests pin AC1 (peak concurrency ≤ 4 with 10 calls /
  50 ms each), AC2 (4-call mixed-latency ordering), AC6 (Error::cancelled
  with `reason=timeout` plus tool / per_call_timeout_ms context), and
  partial AC5 (parent terminal cancel returns `Error::cancelled` with
  `reason=parent_cancelled`). Slice 117 adds AC3 (two writes to the same
  workspace-resolved path serialize via the exclusive lock; peak in-flight
  = 1), AC4 (concurrent read + write on the same path do not overlap;
  peak in-flight = 1; shared+exclusive counters both move), the shared-lock
  parallel-read case (peak in-flight = 2 with both calls under shared
  acquires), the distinct-path parallel-write case (peak in-flight = 2;
  two exclusive acquires, zero contention), the capability-free fall-through
  case (no lock taken when `required_capabilities` is empty), `reap_idle_locks`
  drops entries past the TTL (and leaves entries within TTL alone), AC10
  via a direct `PathLockTable` exercise of 10 000 distinct paths + reap
  past TTL — all entries evicted, `stats().current_entries == 0`,
  `stats().reaped_entries == 10 000` — and a cancellation-during-wait case
  that holds an exclusive lock, queues + cancels a second exclusive
  waiter, then verifies a fresh acquire on the same key succeeds after the
  original holder releases (no orphaned permit). Slice 115 closes the 2026-05-21
  deep-review P1 `tool::parse_input<T>` cleanup by extracting
  `tool::detail::parse_input_object(input_json, tool_name)` and
  `tool::detail::require_string_field(input, tool_name, field)` into
  `src/oran-tool/_impl/parse_input.hpp` and rewriting all seven built-ins
  (`file.read`, `file.write`, `file.edit`, `file.delete`, `file.search`,
  `directory.list`, `tool.search`) to consume them. The shared helpers
  standardise the error vocabulary — `"<tool>: input is not valid JSON"`
  (with `detail`), `"<tool>: input must be a JSON object"`, and
  `"<tool>: input must include a string `<field>` field"` — so three built-ins
  (`file.read`, `file.delete`, `directory.list`) that previously combined the
  object + path checks now report the two failure modes as separate, distinct
  errors. The seven refactored built-ins keep their existing test coverage
  unchanged; the new helper TU adds direct unit coverage at
  `tests/tool/test_parse_input.cpp` (7 cases, 28 assertions). Focused result:
  `test-tool` 178 / 1838 → **185 / 1866** (+7 cases, +28 assertions); the
  other 13 test suites are unchanged. Slice 114 bundled the
  remaining 2026-05-26 deep-review fixes;
  (F1+F18) trace-write failures no longer mask the loop's underlying error —
  `agent::Loop` attaches `trace_write_failed=<message>` context to the
  original provider/tool/loop-boundary error instead of returning the
  storage error in its place; (F5) `provider::Response` now carries
  `route_profile_used`, `provider::execution::Runtime` fills it with the
  served target's profile (mirroring how `model_used` is filled), and the
  loop reads it so `trace_turns.route_profile` describes the profile that
  actually answered the request even when a `Route::fallbacks` entry won;
  (F6 + F12) `bootstrap::run` parse_args now rejects single-dash short
  flags as `--audit-init` paths (so `--audit-init -h` no longer creates a
  `-h` directory) and rejects duplicate `--config / --audit-init / --trace`
  occurrences with `invalid_argument`; (F9) the agent loop's terminal arm
  treats `core::StopReason::cancelled` as terminal-success so an OpenAI
  Responses `status="cancelled"` returns through the success path with a
  cancelled trace row instead of the `non-terminal stop reason` error
  branch; (F10) the provider execution wrapper enriches retry-backoff
  sleep errors with `with_target_context` so cancellation-during-backoff
  carries the same `provider_profile / provider_model / attempt /
  max_attempts` context every other early-exit attaches; (F11) the OpenAI
  Responses encoder folds `core::Role::system` messages into
  `body.instructions` (matching the Anthropic side) so a system message
  inside `request.messages` no longer double-emits as `input[].role==system`;
  (F3 + F4 + F23) the `oran-io` singleflight leader now wakes follower
  `steady_timer`s by posting `expires_at` onto each waiter's own executor
  (closing the asio "shared objects: Unsafe" race) and a RAII guard
  publishes a `singleflight_leader_unwound` cancelled result on any leader
  unwind path so a cancelled cold-read can no longer orphan followers
  forever; the redundant pre-cold `co_await asio::post` is removed.
  Style cleanups land alongside the bug fixes: (F14) `static_cast<void>`
  replaces the C-style `(void)value;` in `oran-config`, (F15) `signal_name`
  returns `std::string_view` instead of `const char*` and the signal-drain
  docstring no longer references a nonexistent helper (F13), (F16/F20)
  `AgentPromptRunner::create` uses `std::make_unique` with a passkey-tagged
  public constructor instead of raw `new`, (F19) the runner's
  `validate_options` now rejects an empty `asio::any_io_executor`, (F22)
  `core::BoundedCache::put` introduces a new `EvictionReason::invalidated`
  so a same-key overwrite or oversize rejection no longer double-counts in
  `evictions_bytes` / `evictions_lru`, (F24) `permission::ApprovalBroker::reap_expired`
  uses `std::erase_if` instead of a hand-rolled iterator erase loop, and
  (F25) `storage::TraceRepository` drops its local `is_zero_id` in favour
  of the shared `core::is_zero_turn_id` helper. Docs (F8 spec 0018) now
  describe ordinary configured-route binary handoff as shipped through
  slice 112 instead of downstream. Focused result across affected libs:
  `test-agent` 26 cases / 407 assertions, `test-bootstrap` 72 cases / 316
  assertions, `test-provider` 66 cases / 528 assertions; the other 11 test
  suites are unchanged. Slice 113 closes the
  long-standing gap that left `agent::SessionState::observe_tool_output(...)`
  unwired in `bootstrap::AgentPromptRunner`: the production runner now walks
  each turn's new transcript suffix, reconstructs a minimal `tool::Output` from
  every `ToolResultContent` block matched to its assistant `ToolUseContent`,
  and feeds the pair through `SessionState::observe_tool_output(name, output,
  now())`. `SessionState` keeps filtering to `tool::kToolSearchName`, so only
  successful `tool.search` payloads trigger deferred-tool promotion; the runner
  exposes a new `tool_search_observations_recorded()` accessor for diagnostics.
  `validate_options` also now rejects a default-constructed `asio::any_io_executor`
  at create time so an empty executor cannot silently propagate into
  `tool::DispatchContext::executor`. Focused result: `test-bootstrap` 70 cases /
  308 assertions, including a scripted `tool.search` round trip that asserts the
  observation counter and a creation-time empty-executor rejection. Slice 112 closes
  the provider adapter v1 binary handoff: when config declares a `default`
  provider route, regular `bootstrap::run` now builds `HttpProviderBackend`
  on the process runtime's CPU executor, creates `AgentPromptRunner`, and
  calls `cli::run_async` so ordinary `--prompt` runs drive `agent::Loop`
  through the configured Anthropic Messages / OpenAI Responses HTTP-backed
  provider system. Built-in empty defaults still report `provider route: none
  configured` and preserve the deterministic no-runner CLI shell, so fresh
  checkouts remain runnable without credentials. Configured-route startup now
  reads the named API-key environment variables at the explicit credential
  boundary, constructs the HTTP transport and adapter system, surfaces missing
  credentials as `ErrorKind::auth` with only non-secret context, and uses
  `runtime.request_timeout_ms` for provider body requests. Focused result:
  `test-bootstrap` 68 cases / 297 assertions, including a localhost
  Anthropic Messages prompt round trip through the ordinary binary handoff and
  a missing-credential error before CLI async execution. Slice 111 added
  the bootstrap-owned HTTP provider backend construction seam needed for that
  handoff. New
  `<oran/bootstrap/provider_backend.hpp>` exports `HttpProviderBackendOptions`
  and movable `HttpProviderBackend`. `HttpProviderBackend::build(config,
  options)` resolves the configured route profiles, builds the adapter plan,
  reads the configured API-key environment variables, owns an `http::Client`
  on the caller-provided blocking executor, adapts that client to
  `provider::ProtocolTransport`, registers the built-in Anthropic Messages and
  OpenAI Responses protocol factories, and returns a profile-routed
  `provider::System` plus the resolved route for `AgentPromptRunner` callers.
  The transport adapter converts protocol HTTP requests into `http::BodyRequest`
  with a positive request timeout and maps `http::Client` failures back into
  provider transport errors without logging secret header values. Focused
  result: `test-bootstrap` 67 cases / 287 assertions, including a localhost
  Anthropic Messages round trip through libcurl and a missing-credential
  construction error with only non-secret context. Slice 110 adds
  the platform-owned `oran-http` target and a libcurl-backed
  body-response client needed before provider factories can use real HTTP/TLS
  I/O. New `<oran/http/client.hpp>` exports stdlib-shaped `Header`,
  `BodyRequest`, `BodyResponse`, and pimpl-backed `http::Client`. The client
  accepts a caller-owned blocking executor (production callers should use
  `async::Runtime::cpu_executor()`), validates method/scheme/timeout before
  curl dispatch, collects response headers/body, maps curl transport failures
  into `core::Error` categories, and returns `ErrorKind::cancelled` when the
  parent cancellation slot is already or becomes cancelled during the curl
  poll loop. The target links system `libcurl >=8.11.0`; curl handles stay
  private to `src/oran-http/client.cpp`. Focused result: `test-http` 3 cases /
  21 assertions. Slice 109 adds
  the provider-owned protocol transport adapter seam needed before concrete
  HTTP/TLS transport and ordinary binary handoff. New
  `<oran/provider/protocol_transport.hpp>` exports the HTTP-shaped
  `ProtocolHttpRequest` / `ProtocolHttpResponse` value types, abstract
  `ProtocolTransport`, `ProtocolTransportAdapterFactory`, and
  `protocol_transport_factory_bindings(anthropic, openai)`. The factory builds
  Anthropic Messages or OpenAI Responses `provider::System` backends from
  resolved credential targets, composes slice 107's request serializer with
  slice 108's response decoder, injects provider API-key headers, maps HTTP
  status classes into provider error categories, rejects mismatched selected
  route profile/model/protocol values before sending, and remains
  offline-testable through a fake transport. That slice's seam was body-response
  only and forced `request.stream=false`; SSE streaming and `oran-http`/libcurl
  I/O remained downstream at that point. `bootstrap::run` still does not
  read provider credentials, construct adapters, allocate a concrete transport,
  send a network request, or start `agent::Loop` for ordinary binary prompts.
  Focused result: `test-provider` 63 cases / 512 assertions. Slice 108 adds
  the provider-owned offline protocol response decoding boundary needed before
  HTTP transport-backed factories. New `<oran/provider/protocol_response.hpp>`
  exports `provider::decode_protocol_response(body_json, target)`. The decoder
  supports `ProtocolKind::anthropic_messages` and
  `ProtocolKind::openai_responses`, keeps `nlohmann_json` private to the
  provider `.cpp`, maps vendor text, thinking/reasoning summaries, tool-use
  blocks, model ids, token usage, and status/stop reasons into the typed
  `provider::Response` contract, maps unknown vendor stop/status values to
  `StopReason::error`, and rejects malformed JSON or unsupported item shapes
  as `ErrorKind::parsing` with non-secret context. `bootstrap::run` still does
  not read provider credentials, construct adapters, allocate transport, send a
  network request, or start `agent::Loop` for ordinary binary prompts. Focused
  result: `test-provider` 57 cases / 442 assertions. Remaining handoff work after
  that slice was transport-backed Anthropic/OpenAI protocol factories and switching
  `bootstrap::run` to `cli::run_async` only when that backend exists. Slice 107 adds
  the provider-owned offline protocol request serialization boundary needed
  before HTTP transport. New `<oran/provider/protocol_request.hpp>` exports
  `provider::ProtocolRequest { method, path, body_json }` and
  `provider::make_protocol_request(request, target)`. The mapper supports
  `ProtocolKind::anthropic_messages` and `ProtocolKind::openai_responses`,
  converts the typed `provider::Request` / `core::Message` / `core::Content`
  contract into vendor JSON body bytes, validates opaque tool schema,
  tool-input, and structured tool-result JSON in the provider `.cpp`, maps
  text-only tool results to the existing text fallback, maps
  `ToolResultContent::data_json` into Anthropic `tool_result.content[]` or
  serialized OpenAI Responses `function_call_output.output`, and rejects
  unsupported protocol families as `Error::config`. `core::ToolResultContent`
  now preserves optional `data_json`, and `agent::Loop` copies successful
  `tool::Output::data_json` into the provider-facing tool-result transcript
  so spec-0014 structured bytes reach the protocol mapper. `oran-provider`
  now uses `nlohmann_json` privately for serialization; public headers still
  expose only bytes and stdlib value types. `bootstrap::run` still does not
  read provider credentials, construct adapters, allocate transport, send a
  network request, or start `agent::Loop` for ordinary binary prompts.
  Focused results: `test-core` 71 cases / 455 assertions, `test-provider`
  51 cases / 398 assertions, and `test-agent` 25 cases / 401 assertions.
  Remaining handoff work after that slice was response decoding, transport-backed
  Anthropic/OpenAI protocol factories, and switching `bootstrap::run` to
  `cli::run_async` only when that backend exists. Slice 106 adds
  the provider-owned adapter factory dispatch seam that consumes slice 105's
  credential bundle without introducing HTTP transport. New
  `<oran/provider/adapter_factory.hpp>` exports
  `provider::ProtocolAdapterFactory`,
  `provider::ProtocolAdapterFactoryBinding`, and
  `provider::make_adapter_system(credentials, factories)`. The factory builds
  one concrete backend per primary/fallback credential target by matching each
  target's `adapter_name` to a caller-registered protocol factory, rejects
  missing/null/duplicate bindings and duplicate route profiles as
  `Error::config`, and returns a profile-routed `provider::System`. That
  returned system expects the execution layer to pass a single selected target
  per call, forwards a one-target route to the matching backend, and leaves
  retry/fallback ownership in `provider::execution::Runtime`. `bootstrap::run`
  does not call this boundary yet, so ordinary startup still preflights
  route/profile/adapter metadata without reading provider credentials,
  decrypting secrets, allocating an HTTP client, constructing a real adapter,
  sending a network request, or starting `agent::Loop` for ordinary binary
  prompts. Focused result: `test-provider` 45 cases / 329 assertions.
  Remaining handoff work is still implementing concrete Anthropic/OpenAI
  protocol factories, wiring transport, and switching `bootstrap::run` to
  `cli::run_async` only when that backend exists. Slice 105 adds
  the explicit provider credential-resolution boundary that a future concrete
  adapter factory will call after slice 104's offline plan. New
  `<oran/provider/credentials.hpp>` exports
  `provider::AdapterCredentialTarget`, `provider::AdapterCredentialBundle`,
  and `provider::resolve_adapter_credentials(plan)`. The resolver reads the
  environment variables named by each plan target's `api_key_env`, stores only
  in-memory API-key strings beside the existing adapter-plan target, derives
  the same loop-facing `provider::Route`, returns `ErrorKind::auth` for
  missing or empty API-key env vars, and keeps error context to non-secret
  fields (`role`, `profile`, `api_key_env`). `bootstrap::run` does not call
  this boundary yet, so ordinary startup still preflights route/profile/adapter
  metadata without reading provider credentials, decrypting secrets, allocating
  an HTTP client, constructing an adapter, sending a network request, or
  starting `agent::Loop` for ordinary binary prompts. Focused result:
  `test-provider` 36 cases / 259 assertions. Remaining handoff work is still
  constructing real Anthropic/OpenAI provider systems from config and switching
  `bootstrap::run` to `cli::run_async` only when that backend exists. Slice 104 adds
  the offline provider adapter construction plan that consumes slice 103's
  route-profile bundle. `<oran/provider/adapter_plan.hpp>` now exports
  `provider::AdapterConstructionTarget`, `provider::AdapterConstructionPlan`,
  and `provider::make_adapter_construction_plan(resolution)`. The plan keeps
  each resolved `ResolvedProfileTarget` beside the protocol adapter name that
  a future concrete factory will dispatch on, derives the existing loop-facing
  `provider::Route`, and preflights non-empty provider/model/base-url/API-key
  env metadata plus `http://` / `https://` endpoint schemes. `bootstrap::run`
  now resolves the configured `default` route profiles and builds this offline
  plan before CLI handoff, preserving the same non-secret startup summary; it
  still does not read environment variables, decrypt credentials, allocate an
  HTTP client, construct an adapter, send a network request, or start
  `agent::Loop` for ordinary binary prompts. Focused result:
  `test-provider` 32 cases / 233 assertions and `test-bootstrap` 65 cases /
  269 assertions. Remaining handoff work is still constructing real
  Anthropic/OpenAI provider systems from config and switching `bootstrap::run`
  to `cli::run_async` only when that backend exists. Slice 103 adds
  the route-profile resolution bundle that real provider adapter construction
  needs after the slice-102 protocol seam. `<oran/provider/route_resolver.hpp>`
  now exports `provider::ResolvedProfileTarget`,
  `provider::RouteProfileResolution`, and
  `provider::resolve_route_profiles(config, route_name)`. That richer resolver
  performs the same route/profile/protocol validation as `resolve_route`, but
  keeps the profile endpoint metadata (`provider`, `base_url`, `api_key_env`)
  beside the loop-facing `ModelTarget`; `RouteProfileResolution::route()`
  derives the existing `provider::Route` so `agent::Loop` and
  `provider::execution::Runtime` do not change. `bootstrap::run` now preflights
  that adapter-factory-ready bundle for the configured `default` route while
  preserving the existing non-secret startup summary; it still does not read
  environment variables, decrypt credentials, construct an adapter, send a
  network request, or start `agent::Loop` for ordinary binary prompts. Focused
  result: `test-provider` 28 cases / 210 assertions. Remaining handoff work is
  still constructing real Anthropic/OpenAI provider systems from config and
  switching `bootstrap::run` to `cli::run_async` only when that backend exists.
  Slice 102 closes
  the provider-profile protocol seam that was still implicit after the slice-98
  route resolver. `config::ProfileConfig` now carries optional
  `protocol`; `oran-config` parses `profiles.<name>.protocol` as a
  non-empty string, `config.example.json` documents
  `"protocol": "anthropic_messages"` on the default profile, and
  `provider::resolve_route` prefers that explicit exact `ProtocolKind`
  spelling before falling back to provider-label aliases such as `anthropic`,
  `openai`, or `deepseek`. That lets a custom/self-hosted vendor label resolve
  to a shipped wire format without overloading `provider`, while preserving the
  older alias path for existing profiles. Invalid explicit protocols now fail
  with `Error::config` carrying `route` / `profile` / `role` / `protocol`
  context during the same bootstrap preflight path that already caught unknown
  provider labels. Focused result: `test-config` 33 cases / 241 assertions,
  `test-provider` 26 cases / 181 assertions, and `test-bootstrap` 64 cases /
  264 assertions. No provider credentials are read, no adapter is constructed,
  no network request is sent, and ordinary binary prompts still do not start
  `agent::Loop`; remaining handoff work is still constructing real
  Anthropic/OpenAI provider systems from config and switching
  `bootstrap::run` to `cli::run_async` only when that backend exists. Slice 101 adds
  the adapter-neutral bootstrap runner that consumes the slice-100 CLI seam.
  `<oran/bootstrap/prompt_runner.hpp>` now exports
  `AgentPromptRunnerOptions` and `AgentPromptRunner`, a caller-supplied
  `cli::PromptRunner` implementation that borrows a `RuntimeAssembly`,
  config, provider backend, executor, and resolved `provider::Route`.
  `AgentPromptRunner::create` registers the shipped builtin tool catalog,
  materializes permissions from config plus an optional agent overlay, wraps
  the backend in `provider::execution::Runtime`, binds
  `cli::OperatorPromptSink` to the assembly-owned `permission_ask_rendered`
  bus with scripted-answer support for tests, threads workspace/audit/broker/
  hook/output-cap services into `tool::DispatchContext`, carries the
  assembly-owned `TraceRepository` into `RunTurnInputs::trace`, and drives
  `agent::Loop` for each parsed prompt while retaining the transcript across
  calls. Focused result: `test-bootstrap` 63 cases / 259 assertions, including
  CLI-to-loop handoff with trace row persistence, provider retry through the
  execution wrapper, and a broker-backed `file.read` approval through the CLI
  sink. Regular `bootstrap::run` still calls the deterministic no-runner
  `cli::run` path until a real provider adapter factory exists, so no provider
  credentials are read, no network request is sent, and the shipped binary
  still does not start `agent::Loop` on ordinary `--prompt` runs. Remaining
  handoff work: construct real Anthropic/OpenAI provider systems from config
  and switch `bootstrap::run` to `cli::run_async` only when that backend exists.
  Slice 100 opens
  the adapter-neutral CLI prompt-runner handoff seam. `<oran/cli/cli.hpp>`
  now exports `PromptRunRequest`, `PromptRunResult`, `PromptRunner`, and
  `cli::run_async(CliOptions, PromptRunner*)`; `run_async` reuses the existing
  mode parser, delegates single-shot prompts and non-empty scripted REPL lines
  to the caller-owned runner in order, prints non-empty runner text when not
  quiet, and propagates runner errors unchanged. `cli::run` remains the
  deterministic no-runner shell, and `bootstrap::run` still calls that path, so
  no provider credentials are read, no adapter is constructed, no network
  request is sent, and ordinary binary prompts still do not start
  `agent::Loop`. Focused result: `test-cli` 14 cases / 97 assertions. Slice
  101 consumes that seam with the bootstrap-owned runner described above.
  Slice 99 consumes
  the route resolver at the binary boundary. Regular `bootstrap::run` startup
  now preflights the configured `default` provider route whenever config
  declares routes, prints
  `provider route: default primary=<profile>/<model> protocol=<kind>
  fallbacks=<n>` plus ordered fallback rows, and returns the resolver's
  `Error::config` before CLI handoff when route/profile/protocol references are
  invalid. Built-in empty defaults still report `provider route: none
  configured` and continue to the deterministic pre-loop CLI shell; no provider
  credentials are read, no adapter is constructed, no network request is sent,
  and ordinary binary prompts still do not start `agent::Loop`.
  `oran-bootstrap` now declares its direct `oran-provider` dependency. Focused
  result: `test-bootstrap` 59 cases / 230 assertions. Slice 101 now supplies
  the runner that wraps caller-provided provider systems in
  `provider::execution::Runtime`; the remaining real-CLI work is provider
  adapter construction and switching `bootstrap::run` to the async handoff.
  Slice 98 lands
  the config-to-provider route resolver required before loop/binary handoff.
  `<oran/provider/route_resolver.hpp>` exports
  `provider::resolve_route(const config::Config&, std::string_view)`, which
  resolves a named `config::RouteConfig` into the existing `provider::Route`
  value by looking up the primary/fallback `config::ProfileConfig` entries,
  preserving authored fallback order, mapping provider spellings and exact
  `ProtocolKind` names into `ProtocolKind`, and returning `Error::config`
  with `route` / `profile` / `role` context for missing references or unknown
  provider spellings. Slice 102 adds optional `profiles.<name>.protocol` so the
  resolver can use an explicit wire-format spelling before provider-alias
  inference and can report unknown explicit protocols with `protocol` context.
  The current typed config still carries only provider/model/base URL/API-key
  metadata plus that optional protocol field, so resolved targets fill
  `{profile, model, protocol}` and leave `thinking_budget` / `cache` unset
  until those route/profile policy fields land. `oran-provider` now declares
  its direct `oran-config` dependency instead of leaning on the transitive
  `oran-prompt` path, and `<oran/provider.hpp>` re-exports the resolver.
  Focused result through slice 102: `test-provider` 26 cases / 181 assertions.
  Remaining
  provider work from that point was provider request/response hooks,
  usage/cost rollups, real Anthropic/OpenAI adapters, and binary construction
  of a concrete provider backend for the bootstrap runner; later slices have
  since shipped the concrete backend, lifecycle hooks, trace-derived usage
  rollup reads, and profile-priced cost calculation, leaving remaining protocol
  families as follow-ups. Slice 103 adds
  `provider::resolve_route_profiles` as the adapter-factory-ready companion to
  `resolve_route`: it preserves `provider`, `base_url`, and `api_key_env` for
  the primary/fallback profiles while deriving the same loop-facing `Route`
  value. Focused result through slice 103: `test-provider` 28 cases / 210
  assertions. Slice 97 lands the first provider execution layer required before
  real adapter and binary handoff work. `<oran/provider/execution.hpp>` now
  exports
  `provider::execution::Runtime`, a `provider::System` decorator over any
  backend `System`. It consumes `Request::retry.max_attempts` and
  `initial_backoff`, retries retryable `network` / `rate_limit` / `timeout` /
  `upstream` errors on the same target, stops immediately for non-retryable
  errors and cancellations, and after a retryable primary exhaustion tries
  `Route::fallbacks` in order with the same per-target attempt budget. Each
  concrete backend call receives a single-target `Route`, so adapters do not
  implement fallback themselves. Successful responses that omit `model_used`
  are filled with the selected target model, preserving later trace rows when a
  fallback wins. Backoff uses `async::sleep_for` and observes parent
  cancellation. If an attempt has already emitted visible `EventSink` output,
  later retryable errors return immediately with `retry_skipped` /
  `fallback_skipped=stream_already_emitted` so terminal/UI callers do not see
  duplicate stream bytes. `test-provider` adds offline execution coverage for
  same-target retry, fallback success, provider-supplied `model_used`,
  non-retryable stop, stream-output retry suppression, zero-attempt validation,
  and cancellation during retry backoff. Slice 96 closed the
  agent-loop approval-observability gap that sat between the direct
  dispatch ask bridge and the later binary handoff. `agent::Loop` now wraps
  each direct `tool::Registry::dispatch` with a scoped dispatch context that
  installs the loop's trace parent id and refreshes `DispatchContext::now` from
  `core::time::now_utc()`, then restores the caller's reusable `parent_turn_id`
  and `now` values after the call. That makes `PermissionAskRenderedPayload`
  `requested_at`, approval-token expiry, and immediate broker verification use
  the real per-tool-call clock instead of a stale caller value such as the
  default epoch. `test-agent` adds an offline fake-provider turn that asks for
  `file.read`, flows through `permission::ApprovalBroker` plus a blocking
  `hook::InProcessSink` on `permission_ask_rendered`, asserts the prompt
  payload's identity/replay/TTL/request time, records
  `metadata_json.permission_ask_decisions[]`, returns the approved tool result
  to the second provider request, and verifies the issued token against that
  request time. Focused result: `test-agent` 24 cases / 391 assertions. The
  remaining approval work is still binary handoff: bind the existing
  `cli::OperatorPromptSink` into the real CLI agent-loop runtime once
  `orangutan` constructs `agent::Loop` with provider/assembly services. Slice
  95 closed spec-0015's first concrete user-visible approval renderer.
  `oran-cli`
  now exports `cli::OperatorPromptSink` from
  `<oran/cli/operator_prompt_sink.hpp>` and the umbrella `<oran/cli.hpp>`.
  The sink implements `hook::Sink`, handles blocking
  `Event::permission_ask_rendered` payloads, renders the tool name, caller
  identity, matched decision reason, replay/TTL policy, request timestamp,
  and input JSON, then accepts yes/approve/proceed or no/deny/reject style
  answers. Approval returns `HookDecisionKind::proceed` with
  `operator_approved:<identity>` in the sink trace; denial returns
  `HookDecisionKind::veto` with `operator_denied:<identity>`. Test and
  noninteractive callers can provide `scripted_answers`; otherwise the sink
  reads one terminal line through an asio `posix::stream_descriptor` on the
  current coroutine executor. `oran-cli` now depends on `oran-async` and
  `oran-hook`. Focused result: `test-cli` 10 cases / 68 assertions. The
  remaining approval work is binary handoff: bind this sink into the real
  CLI agent-loop runtime once `orangutan` drives `agent::Loop` with a real
  provider adapter. Slice 94 closed the direct-dispatch half of
  spec-0015's `permission_ask_rendered` round-trip.
  `<oran/hook/payload.hpp>` now carries
  `hook::PermissionAskRenderedPayload { tool_name, input_json, who,
  decision_reason, replay_max, approval_ttl, requested_at }` in the public
  `hook::Payload` variant. `tool::DispatchContext` gains an optional
  `approval_token_output` slot. When a permission rule returns `ask`, a
  broker is present, no caller-supplied token exists, and a bus is attached,
  `Registry::dispatch` publishes blocking
  `Event::permission_ask_rendered`. A subscribed sink returning `proceed`
  issues a broker grant using the matched rule's TTL/replay policy, stores
  the token for the caller when requested, immediately verifies it, records
  `outcome=approved`, and runs the handler. A sink returning `veto` records
  `outcome=rejected`, `reason=operator_denied`, skips the handler, and returns
  `Error::permission_denied` with `reason=operator_denied` plus the sink
  reason as `hook_reason`. Unsupported ask decisions (`rewrite` /
  `require_approval`) are rejected the same way, and buses with no subscribed
  ask sink preserve the legacy `approval_required` short-circuit. Permission
  ask sink traces are serialized under
  `metadata_json.permission_ask_decisions[]`. Focused result: `test-tool` 178
  cases / 1838 assertions.
  Slice 93 closes
  spec-0018 AC5 for direct `tool_before` blocking publishes. The audit DB
  migration stream now reaches version 4:
  `src/oran-storage/migrations/audit/0004-audit-event-kind.sql` adds
  `audit_events.event_kind TEXT NOT NULL DEFAULT 'permission_decision'` plus a
  parent-turn index for hook rows. `storage::AppendAuditEventRequest`,
  `UpdateAuditEventMetadataRequest`, `AuditEventRecord`, and
  `ListAuditEventsOptions` expose the discriminator, and update-metadata
  matching includes it so ordinary permission-row enrichment cannot clobber a
  same-tool `hook_publish` row. `permission::AuditEvent` and
  `AuditMetadataUpdate` carry the same field through `RecordingAuditSink` and
  `StorageAuditSink`. `tool::Registry::dispatch` now writes an extra
  `event_kind=hook_publish` audit row after a blocking `tool_before` publish
  when `DispatchContext::parent_turn_id` is set and the bus returned consulted
  sink traces; the row uses the same parent turn id and serializes
  `metadata_json.event`, `sink_id`, `decision_kind`, `reason`, optional
  `elapsed_ms` / `error`, and the full `hook_decisions[]` trace before the
  existing permission-decision row is recorded. `orangutan --trace` already
  joins the new row through `AuditRepository::list_events_for_turn`; its audit
  line now prints `kind=<event_kind>` so operators can distinguish hook
  publishes from permission decisions. Focused results: `test-storage` 72 cases
  / 899 assertions, `test-permission` 89 / 426, `test-tool` 174 / 1769, and
  `test-bootstrap` 57 / 226. Slice 95 closes the v1 operator-prompt sink;
  the remaining spec-0015/0018 item here is the binary handoff that drives
  `agent::Loop` from inside the `orangutan` binary once a real provider
  adapter exists. Slice 92 closes
  spec-0015's direct-dispatch blocking timeout follow-up. `oran-config`
  now parses the top-level `hooks.timeout_ms` policy as a positive integer
  with a default of 2000 ms, `config.example.json` documents that default,
  and `bootstrap::run` threads the parsed value into
  `RuntimeAssemblyOptions::hook_blocking_timeout`. `RuntimeAssembly` now owns
  the process `hook::Bus` alongside the broker/audit/workspace/trace bundle
  and constructs it with `hook::BusOptions{blocking_timeout}`; the startup
  banner reports `hook-timeout=<ms>`. `hook::Bus::publish_blocking<E>` races
  each consulted blocking sink against `async::sleep_for` on the coroutine
  executor. A timed-out sink synthesizes a `veto` with
  `reason=hook_timeout`, records `HookDecisionTrace::elapsed`, short-circuits
  later blocking sinks, and lets direct `Registry::dispatch` reuse the slice-91
  `blocked_by_hook` path: handler skipped, advisory failure events published,
  audit row `outcome=blocked_by_hook`, and
  `metadata_json.hook_decisions[].elapsed_ms` persisted. Focused results:
  `test-config` 32 cases / 235 assertions, `test-hook` 30 / 207,
  `test-bootstrap` 57 / 224, and `test-tool` 173 / 1739. Slice 91 consumes
  spec-0015's blocking `tool_before` surface inside direct
  `tool::Registry::dispatch`: `<oran/hook/decision.hpp>` now carries
  `HookDecisionTrace { sink_id, kind, reason }` and
  `HookDecision::trace`, and `hook::Bus::publish_blocking<E>` fills that
  vector in subscription order for every sink it actually consults. Dispatch
  now calls `publish_blocking<Event::tool_before>` before workspace
  pre-resolution and permission evaluation; `veto`, hook-error, and malformed
  `rewrite` decisions record `permission::AuditOutcome::blocked_by_hook`,
  skip the handler, publish advisory `tool_error` / `tool_after` with
  `error_kind=blocked_by_hook`, and return `Error::permission_denied`.
  Valid `rewrite` decisions substitute the effective input before workspace
  resolution, permission evaluation, broker checks, audit, handler execution,
  and later hook payloads; allowed rewritten calls record
  `AuditOutcome::rewritten`, row `input_hash=SHA-256(rewritten_input)`, plus
  `metadata_json.original_input_hash`, `rewritten_input_hash`, and
  `hook_decisions`. `require_approval` promotes otherwise-allow decisions into
  the existing `ApprovalBroker` path while preserving an underlying permission
  deny. `permission::AuditOutcome` now exports the two new enum values, and
  `bench-hook` adds blocking-publish scenarios for no sinks, one sink,
  three all-proceed sinks, and a second-sink short-circuit. Focused results:
  `test-hook` 29 cases / 196 assertions, `test-permission` 89 / 426, and
  `test-tool` 172 / 1722. Slice 89 closes
  spec-0018 AC12 by adding `bench/storage/scenarios/trace_turn_insert.cpp`,
  a single-insert A-vs-B pair for `trace_turns`:
  `storage.trace_turn_insert_raw_pool` runs one raw `Pool` +
  `StatementCache` INSERT per nanobench iteration and
  `storage.trace_turn_insert_repository` runs one
  `TraceRepository::append_turn` per iteration. Both use unique per-iteration
  turn ids and their own temp DB so the bench measures steady-state
  per-turn cost without batch overhead. Initial WSL2 numbers report about
  13 µs / insert for the raw path and about 16 µs / insert for the
  repository wrapper — both comfortably inside the spec's ≤ 50 µs
  target. Adjacent to the new scenario, the existing
  `scenarios/trace_repository.cpp` (32-row batch) needed an `id_for`
  collision fix: the overlapping-sum encoding `salt + row + i + (batch &
  0x0f)` produced identical bytes for distinct `(batch, row)` tuples — for
  example `(0, 1)` and `(1, 0)` — and the trace `PRIMARY KEY` guard
  aborted the second nanobench epoch. The fix packs `(salt, row, batch)`
  into non-overlapping byte ranges so every tuple maps to a distinct id,
  and the batch scenarios now report stable numbers alongside the new
  single-insert pair. `test-storage` still reports 72 cases / 886
  assertions; the change is bench-only.
  Slice 88 closes spec-0018 AC10 by adding the operator inspector: `oran-storage` exports
  `AuditRepository::list_events_for_turn(TurnId, limit)` — a `parent_turn_id =
  ?` read ordered `id ASC` so the original `tool_use` order of a spec-0017
  multi-tool turn survives the trace/audit join — and `oran-bootstrap`'s
  `--trace <turn-id>` / `--trace=<turn-id>` flag opens the workspace audit
  DB, runs the idempotent audit migration, looks up the trace row through
  `TraceRepository::get_turn`, lists the joined audit rows through the new
  repository method, and prints both in the `--explain-rules`-style line
  format before exiting zero. The inspector returns `Error::not_found` for
  a missing audit DB and for an unknown turn id, propagates SIGINT/SIGTERM
  through the existing `SignalScope` so the one-shot `io_context` drains
  promptly, and accepts the 32-char lowercase hex spelling that storage
  round-trips through `BLOB`. `test-storage` now reports 72 cases / 886
  assertions and `test-bootstrap` reports 56 cases / 221 assertions. Hook
  publish rows, the bench `trace_turn_insert` scenario, and the binary
  handoff that drives `agent::Loop` from inside the binary remain
  downstream. Slice 87 closed
  the first downstream item on the spec-0018 punch list by threading
  `config.trace().enabled` from `oran-config` through `bootstrap::run` into
  the new `RuntimeAssemblyOptions::trace_enabled` switch and constructing
  a `storage::TraceRepository` on the assembly-owned audit `Pool` when
  both audit and trace are enabled. `RuntimeAssembly` now exposes
  `trace_repository()` (non-null only when `trace_enabled()` returns true)
  so the upcoming agent-loop owner can plug the repository into
  `agent::TraceContext` without minting a second DB handle. The bootstrap
  startup banner reports `trace=enabled|disabled` alongside the existing
  audit/workspace summary, and `test-bootstrap` now reports 51 cases /
  188 assertions covering the default-on path (smoke `append_turn`),
  explicit trace-off, and audit-disabled-forces-trace-off cases. Hook
  publish rows, CLI `--trace`, and binary handoff remain downstream.
  Slice 86 closes
  the last loop-owned spec-0018 writer gap by persisting iteration-cap exits.
  When `LoopOptions::max_iterations` is exhausted by repeated tool_use
  responses and `RunTurnInputs::trace` has an enabled `TraceRepository`,
  `agent::Loop` now writes a body-free `trace_turns` row with
  `stop_reason=error`, `iteration_count = LoopOptions::max_iterations`, the
  final iteration's rendered prompt prefix hash/bytes and active/deferred
  catalog hashes, the aggregated provider usage, and the last response's
  model id (falling back to the primary route model when the final response
  omitted one). The existing `Error::internal` with `reason=iteration_cap`
  is still returned unchanged afterwards, and trace-disabled or
  repository-less callers still take the legacy no-row path.
  `test-agent` now reports 23 cases / 363 assertions. Config-to-loop wiring,
  hook publish rows, CLI `--trace`, and binary handoff remain downstream.
  Slice 85 lands
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
  hook publish rows, CLI `--trace`, and binary
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
  Iteration-cap trace rows, approval-observability coverage, and the
  trace/audit inspector rows are now in place; slice 97 adds the provider
  execution retry/fallback decorator, and slice 101 consumes that decorator
  through bootstrap's `AgentPromptRunner` for caller-supplied backends while
  the parallel `ToolScheduler` and ordinary CLI/binary handoff with real
  adapters remain downstream. Slice 76
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
  existing `LoopOptions::max_iterations` cap with `reason=iteration_cap`,
  writes iteration-cap trace rows when tracing is configured, and refreshes
  `DispatchContext::now` around direct dispatch so broker-backed
  `permission_ask_rendered` approvals use the real per-call clock. Provider
  retry/fallback and the parallel `ToolScheduler` remain downstream. `test-agent`
  now covers the FakeProvider text-turn path,
  provider request mapping, provider error forwarding, the no-dispatch-context
  tool-use boundary, one-tool provider re-entry, ordered multi-tool results,
  model-visible missing-tool repair, infrastructure error propagation, and the
  iteration cap, provider/tool cancellation trace rows, and provider/loop-boundary
  error trace rows, and the fake-provider approval-clock path. The
  `orangutan` binary is still not wired to `oran-agent`; remaining near-term
  work is CLI/binary handoff.
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
  fixture (`agent.prompt_cache_no_promotions` about 56.6 us / fixture,
  `agent.prompt_cache_after_promotion` about 63.0 us / fixture) and aborts
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
  effort under the active exec plan; its next code step is adapting the
  shipped `oran-http` body client into `provider::ProtocolTransport`
  during bootstrap construction. Binding the CLI approval sink into real turns is still gated on
  the provider-backed `oran-agent` handoff; and wiring
  `check-compile-budget.sh` into
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
| **C** | Compile-time discipline, Tests, Benches, IO, Storage, Config, Bootstrap, Provider system, Tool registry, Prompt builder, Memory tiers, Permissions, Hooks, Channels, Orchestration, Automation, Desktop App, CLI, Skills, Static analysis |
| **D** | Observability |

## Latest Library Surfaces

- `oran-core`: 71 cases / 459 assertions.
- `oran-async`: 11 cases / 51 assertions.
- `oran-http`: 3 cases / 21 assertions.
- `oran-io`: 54 cases / 311 assertions.
- `oran-storage`: 77 cases / 988 assertions.
- `oran-config`: 48 cases / 429 assertions.
- `oran-permission`: 89 cases / 426 assertions.
- `oran-hook`: 37 cases / 299 assertions.
- `oran-memory`: 38 cases / 841 assertions.
- `oran-skill`: 27 cases / 168 assertions.
- `oran-tool`: 208 cases / 2181 assertions.
- `oran-prompt`: 10 cases / 98 assertions.
- `oran-provider`: 86 cases / 652 assertions.
- `oran-agent`: 56 cases / 10 744 assertions.
- `oran-cli`: 26 cases / 205 assertions.
- `oran-bootstrap`: 124 cases / 1054 assertions.

## Open Tech-Debt Rows

Lifted from [`exec-plans/tech-debt-tracker.md`](exec-plans/tech-debt-tracker.md).
Closed entries do *not* live here — the tracker is canonical.

- 2026-05-26 — Deep-review backlog `review/deep-2026-05-26`: slices 113 and
  114 absorbed the high/medium bullets (F1, F2, F5, F6, F9, F10, F11, F12,
  F18, F19) and most low-severity items (F3 + F4 + F23 oran-io singleflight
  cancel + cross-thread wake; F8 spec 0018 binary-handoff sync; F13/F15
  signal_drain docs + return type; F14 (void) discard; F16/F20 raw new in
  prompt runner factory; F22 BoundedCache double-count; F24 erase_if;
  F25 duplicate helper). Remaining: a future regression test for the
  oran-io singleflight leader-cancel + cross-executor-wake fix; rebench
  any cache stat consumers that previously read `evictions_bytes` /
  `evictions_lru` after invalidations now that those evictions move to
  `EvictionReason::invalidated` (no callers do today, but a future
  observability consumer should be aware).
- 2026-05-21 — Second deep-review follow-up
  `review/deep-2026-05-21-followup`: slice 151 closed the config strictness
  sweep for typed nested provider/route/hook sections, slice 152 closed
  redacted default hook payloads for `file.write` / `file.edit`, and slice 153
  closed atomic-write durability (`fsync_file` / parent fsync plus
  cross-process-unique temp leaves). Remaining: CI xmake/test wiring after
  reference hardware is provisioned.
- 2026-05-21 — Deep-review backlog: the stale root review artifact was
  deleted after its actionable findings were absorbed into the tracker and
  specs 0011-0018. Slices 31-36 closed the rank-0 items plus the P0
  follow-ups, slice 60 closed the P2 `tool::Output` envelope item, slice 115
  closed the P1 `tool::parse_input<T>` helper item, slice 154 closed the P2
  public `io::run_blocking` utility item, slice 155 closed the P2
  `DispatchContext::for_now()` factory item, slice 156 closed the P2 parallel
  `publish_advisory` fan-out item, slice 157 closed the P3 storage `Pool`
  contention bench item, slice 158 closed the P3 hook payload-sharing item,
  slice 159 closed the P3 `Runtime::Impl::run()` clarification item, slice 160
  closed the vector backend trait half of the P3 memory follow-up, slice 161
  closed the default FTS5 lexical backend half, slice 162 closed the first
  long-term runtime recall composition half, slice 163 closed bootstrap
  assembly ownership for the default long-term memory DB/backend/runtime,
  slice 164 closed the opt-in prompt-boundary recall rendering path, slice 165
  closed the configured-route recall policy mapping, slice 166 closed the first
  recall kind-filter policy slice, and slice 167 closed the first recall
  query-derivation selector. Slice 168 closed the first read-only long-term
  memory tool by wiring deferred `memory.recall` through the existing
  permissioned tool dispatch path. Slice 169 closed the first write-side memory
  tool by wiring deferred `memory.remember` through that same path. Slice 170
  closed the delete-side long-term memory tool by wiring deferred
  `memory.forget` through the same permissioned dispatch path. Slice 171 added
  the 10k-record FTS5 `longterm::Runtime::search` bench baseline for the P3
  measure-first memory path. Slice 172 closed the first hybrid/vector
  composition contract by adding `memory::longterm::HybridRuntime`, and slice 173
  added the FTS5-vs-vector-vs-hybrid comparison bench over a brute-force cosine
  reference `VectorBackend`; slice 176 closed the gated sqlite-vec adapter, while
  sqlite-vec corpus numbers, embedding/vector ownership, and hybrid ranking
  policy/wiring remain grouped P1/P2/P3 in the tracker.
- 2026-05-20 — `scripts/check-compile-budget.sh` exists and works (slice 28)
  but is not wired into `scripts/ci.sh`. Gated on CI provisioning xmake on
  the documented reference hardware (8-core / NVMe / native Linux);
  otherwise the gate fires on environmental drift, not real regressions.
- 2026-05-17 — `file.search` does not yet ship ripgrep-class optimisations
  (mmap, extension-based binary skip, multi-threaded walk).
  Adequate at slice 20 (~27 µs / 4-file tree) but 3-10× slower than a tuned
  scanner on repo-scale inputs. Re-bench once `oran-agent` produces a real
  workload measurement.
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

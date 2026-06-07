# 0005 — Memory System

## User Problem

Agents that forget everything between turns are useless beyond a single conversation.
The legacy `orangutan/` had a single-tier memory; v2 makes the tiers explicit so
operators can reason about retention, scope, and visibility.

## Scope (v1)

- Storage foundation shipped: `oran-storage::SessionRepository` owns the
  `sessions.db` schema and provides expected-only append/load/get/list operations
  over the cached writer/reader `Pool`; its schema now loads from the checked-in
  SQL migration file under `src/oran-storage/migrations/sessions/`.
- `oran-memory::session::Store` shipped in slice 130 as typed per-session
  conversation history (replaces legacy `SessionStore`) over
  `SessionRepository`. It privately serializes/deserializes `core::Message`
  blocks, keeps `oran-storage` JSON-opaque, validates required ids, and returns
  parsing errors for malformed stored rows.
- Bootstrap runtime ownership shipped in slice 131: configured-route
  `RuntimeAssembly` opens and migrates a separate
  `<workspace>/.orangutan/sessions.db`, exposes `memory::session::Store` through
  `session_store()`, and keeps built-in no-route startup session-memory disabled.
- Bootstrap runner persistence shipped in slice 132: configured-route
  `AgentPromptRunner` now loads session history from `session_store()` before
  each prompt and appends only the successful transcript suffix afterward, while
  the in-process transcript remains the fallback when session memory is
  disabled.
- Prompt memory framing shipped in slice 133: `memory::FramingOwner` owns the
  section-5 memory framing value and `AgentPromptRunner` renders it once before
  `agent::Loop`, preserving the once-per-turn boundary for future recall
  plumbing.
- Long-term backend contract prework shipped in slice 160:
  `memory::longterm::RecordKind` (`user`, `feedback`, `project`, `reference`,
  reserved `team`), `RecordKey`, `Record`, `Query`, `SearchHit`,
  `WriteRequest`, `TouchRequest`, `Backend`, `VectorBackend`, and validation
  helpers now exist in `<oran/memory/longterm.hpp>`.
- SQLite FTS5 long-term backend shipped in slice 161:
  `memory::longterm::Fts5Backend` owns the default lexical schema under
  `src/oran-memory/migrations/longterm/`, migrates it through `storage::Pool`,
  implements scoped `get` / `search` / `upsert` / `touch` / idempotent
  `remove`, filters by kind and shadow state, and returns lexical scores from
  SQLite BM25.
- Runtime recall composition shipped in slice 162:
  `memory::longterm::Runtime` wraps a `Backend`, validates runtime search/recall
  requests before backend dispatch, and returns `RecallResult { hits, framing }`
  with deterministic `memory::Framing` bytes rendered from the returned records.
- Bootstrap long-term ownership shipped in slice 163:
  configured-route `RuntimeAssembly` opens and migrates a separate
  `<workspace>/.orangutan/memory.db`, owns the default `Fts5Backend` plus
  `longterm::Runtime`, and leaves the built-in no-provider route disabled so
  fresh deterministic CLI runs do not create long-term memory state.
- Prompt-boundary long-term recall shipped in slice 164:
  `AgentPromptRunnerOptions::longterm_recall` is an explicit opt-in that queries
  the assembly-owned `longterm::Runtime` once from the current prompt and stable
  scope key, rejects exact `memory_framing` overrides, and feeds deterministic
  record-only recall framing into section 5 before `agent::Loop`.
- Config-driven prompt-boundary recall shipped in slice 165:
  `memory.longterm.recall.enabled` / `limit` parse through `oran-config`, stay
  disabled by default, and ordinary configured-route `bootstrap::run` maps them
  into `AgentPromptRunnerOptions::longterm_recall`. Slice 166 adds optional
  `memory.longterm.recall.kinds`, a non-empty unique array of `RecordKind`
  spellings that constrains the prompt-boundary query while omission preserves
  all-kind recall. Slice 167 adds `memory.longterm.recall.query_strategy`:
  `prompt_text` keeps the current prompt as the search text, while
  `last_user_message` uses the most recent previous user text when a follow-up
  prompt should recall from session context.
- Read-only long-term memory tool shipped in slice 168: `memory.recall` is a
  deferred built-in with `Capability::read_memory`, parses
  `{query, limit?, kinds?}`, delegates through
  `DispatchContext::memory_recall`, and uses the assembly-owned
  `longterm::Runtime` to return deterministic recall text plus structured
  `memory_recall` record metadata.
- Write-side long-term memory tool shipped in slice 169: `memory.remember` is a
  deferred built-in with `Capability::write_memory`, parses
  `{id, kind, title, body, importance?, tags?, linked_record_ids?, shadow?}`,
  delegates through `DispatchContext::memory_remember`, and uses the
  assembly-owned long-term `Backend` to upsert a record scoped to the runner's
  stable scope key with dispatch-time timestamps, returning confirmation text
  plus structured `memory_remember` saved-record metadata.
- Delete-side long-term memory tool shipped in slice 170: `memory.forget` is a
  deferred built-in with `Capability::write_memory`, parses `{id}`, delegates
  through `DispatchContext::memory_forget`, and uses the assembly-owned
  long-term `Backend` to remove the scoped record idempotently, returning
  confirmation text plus structured `memory_forget` removed-key metadata.
  Vector search and hybrid ranking remain downstream.
- Long-term FTS5 search baseline shipped in slice 171:
  `bench/memory/scenarios/longterm_fts5.cpp` seeds a synthetic 10k-record corpus
  once and measures `memory::longterm::Runtime::search("react agent loop",
  limit=10)` through the default `Fts5Backend`. The local slice run reported
  **~15.08 ms / batch**, below the 50 ms acceptance budget and ready for
  sqlite-vec / hybrid comparisons.
- Hybrid search composition contract shipped in slice 172:
  `memory::longterm::HybridSearchRequest` and `HybridRuntime` compose the
  lexical `Backend` with any `VectorBackend` without adding an optional
  dependency. The runtime validates text query, embedding, limits, and
  non-negative weights; searches both backends; hydrates vector-only hits
  through `Backend::get`; ignores stale vector rows with missing records; and
  returns deterministic weighted `SearchHit` rows. `HybridRuntime::recall`
  reuses the shipped recall framing renderer. The gated sqlite-vec adapter ships
  in slice 176, and slice 178 wires gated bootstrap/config consumption.
- Long-term search comparison bench shipped in slice 173:
  `bench/memory/scenarios/search_fts5_vs_vector.cpp` seeds one shared 10k-record
  corpus and times FTS5-only `Runtime::search`, an in-bench brute-force cosine
  `VectorBackend`, and `HybridRuntime::search` over both at `limit=10`. Local run
  (WSL2, release): FTS5-only ~15.42 ms, vector cosine ~675.6 µs, hybrid ~16.18 ms —
  the lexical scan dominates at 10k records, so hybrid ≈ FTS5 + ~5%. The cosine
  backend is an in-process reference; slice 176 adds the gated
  `--vector_memory=y` sqlite-vec adapter satisfying the same `VectorBackend`
  contract, but its corpus bench numbers remain downstream.
- Hybrid-search config contract shipped in slice 174:
  `oran-config` now parses `memory.longterm.hybrid_search.enabled`, positive
  `lexical_limit` / `vector_limit` / `result_limit`, and non-negative finite
  `lexical_weight` / `vector_weight`, rejecting the all-zero weight case so the
  policy matches `HybridRuntime` validation before bootstrap consumes it. The
  block defaults disabled; since slice 178, `--vector_memory=y` configured-route
  startup consumes it for hybrid recall, while default builds still reject
  enabled hybrid search.
- Hybrid-search bootstrap guard shipped in slice 175:
  configured-route `bootstrap::run` first consumed
  `memory.longterm.hybrid_search.enabled` by rejecting `enabled=true` before
  runtime assembly state or provider traffic. Slice 178 narrows that guard to
  default builds, which now report `reason=build_option_disabled` and
  `option=vector_memory`; vector builds consume the policy.
- Gated sqlite-vec adapter shipped in slice 176:
  `memory::longterm::SqliteVecBackend` implements the shipped `VectorBackend`
  contract when xmake is configured with `--vector_memory=y`. It uses
  `SqliteVecBackend::auto_extensions()` with `storage::Pool::open(...)`, migrates
  a scoped sqlite-vec `vec0` table with cosine distance, validates configured
  dimensions against an existing table, and supports scoped upsert/search/remove.
  Default builds still expose the type but return `ErrorKind::config` with
  `reason=build_option_disabled` from migration and vector operations.
- Gated sqlite-vec corpus bench shipped in slice 177:
  `bench/memory/scenarios/search_fts5_vs_vector.cpp` now adds sqlite-vec
  vector-only and FTS5+sqlite-vec hybrid rows when built with
  `--vector_memory=y`, reusing the same deterministic 10k-record corpus keys and
  embeddings as the brute-force vector baseline. Local release run:
  sqlite-vec vector-only **~3.03 ms / batch** and FTS5+sqlite-vec hybrid
  **~18.96 ms / batch** at `limit=10`; default builds still emit only the
  FTS5/brute-force/hybrid comparison rows.
- Bootstrap hybrid recall wiring shipped in slice 178:
  `RuntimeAssembly` can now open a separate
  `<workspace>/.orangutan/memory-vectors.db` sqlite-vec pool/backend and expose
  `longterm_vector_backend()` plus `longterm_hybrid_runtime()`. `bootstrap::run`
  enables that owner when `memory.longterm.hybrid_search.enabled=true` and the
  binary was built with `--vector_memory=y`; `AgentPromptRunner` maps the
  configured hybrid limits/weights into enabled prompt-boundary recall and
  `memory.recall`, using deterministic local text embeddings
  (`oran-local-text-v1`, 64 dimensions). `memory.remember` mirrors saved records
  into vector memory, and `memory.forget` removes the matching vector row.
  Default builds keep sqlite-vec optional and fail fast on enabled hybrid search.
- Memory lifecycle hook wiring shipped in slices 179-180:
  `oran-hook` now carries typed `MemoryWritePayload` / `MemoryForgetPayload`
  plus `MemoryReadPayload` / `MemoryReadHitPayload` shapes. Default sinks
  receive redacted memory metadata; `trusted_local` sinks can inspect raw write
  records and raw recall queries/hit records. `AgentPromptRunner` publishes
  blocking `memory_write_before` before `memory.remember` mutates
  lexical/vector memory; a veto returns a permission-denied tool error and
  skips persistence. Successful memory writes publish advisory
  `memory_write_after`, successful `memory.forget` calls publish advisory
  `memory_forget`, and successful prompt-boundary recall plus `memory.recall`
  tool reads publish advisory `memory_read_after`.
- Recall read-touch metadata shipped in slice 181:
  `memory::longterm::Backend` now includes `touch(TouchRequest)`. The default
  `Fts5Backend` advances `last_read_at` monotonically without rebuilding FTS
  text or changing `updated_at`, and `Runtime::recall` /
  `HybridRuntime::recall` touch returned hits before rendering framing/data.
  Plain `search(...)` remains read-only so callers can still inspect rankings
  without mutating read metadata.
- Decay shadow execution shipped in slice 182:
  `memory::longterm::Backend` now includes `decay(DecayRequest)`. The default
  `Fts5Backend` marks scoped, visible records whose
  `last_read_at < unused_before` and `importance <= importance_floor` as
  `shadow=true` in bounded batches, advances `updated_at` monotonically to
  `decay_at`, syncs FTS shadow metadata, and returns the records it changed.
  Default search hides those rows unless callers set `Query::include_shadow`.
- Retention config policy shipped in slice 183:
  `oran-config` now parses `memory.longterm.retention` with explicit-unit
  fields: positive `forget_after_unused_days`, bounded `importance_floor`
  (`0..1`), positive `max_records_per_scope`, and positive
  `decay_check_interval_hours`. The defaults are 180 days / 0.0 / 10000 / 24 h,
  and the block participates in the existing strict/loose unknown-field
  handling for nested memory config.
- Configured-route startup retention consumption shipped in slice 184:
  `bootstrap::run` maps that retention policy into one
  `LongtermMemoryStartupDecayOptions` pass for the runner's stable `cli` scope,
  and `RuntimeAssembly::build` runs the bounded lexical decay after long-term
  migration and before exposing the long-lived memory backend/runtime. The
  `decay_check_interval_hours` field remains reserved for future periodic
  automation cadence.
- Startup retention diagnostics shipped in slice 185:
  `RuntimeAssembly` now retains the optional startup pass shadow count and
  `bootstrap::run` prints the same value as `startup-decay=<disabled|N>` in the
  startup banner. Operators can tell whether startup decay was disabled, ran
  with zero candidates, or shadowed records without inspecting the memory DB.
- Startup retention hook observability shipped in slice 186:
  `oran-hook` now carries typed `MemoryDecayPayload`, and
  `RuntimeAssemblyOptions::startup_hook_bindings` lets in-process observers
  subscribe before startup producers run. After a configured-route startup
  `Fts5Backend::decay(...)` pass succeeds, bootstrap publishes advisory
  `memory_decay` metadata with source, scope, policy inputs, shadowed count, and
  timing, then unbinds those startup-only observers before returning the
  assembly. The payload contains no decayed record content.
- Periodic retention planning shipped in slice 187:
  `oran-automation` now evaluates periodic schedule state and maps due
  long-term retention jobs into `memory::longterm::DecayRequest`. Bootstrap
  mapping, persistent scheduling, actual backend execution, and periodic
  `memory_decay` publishing remain downstream.
- Bootstrap retention job mapping shipped in slice 188:
  configured-route startup maps `memory.longterm.retention` into an
  automation-owned `MemoryRetentionJob` descriptor and stores it on
  `RuntimeAssembly`. The descriptor's first fire is after the one-shot startup
  decay interval; no background loop, persistence, backend execution, or
  periodic `memory_decay` publishing is started by this mapping.
- Automation retention state persistence shipped in slice 189:
  `oran-automation::AutomationRepository` owns the first `automation.db`
  retention schema above `storage::Pool`, persists the configured job by
  durable `job_key`, stores `last_fired_at`, records success/failure run rows,
  and lists recent runs. Bootstrap still does not open `automation.db`, run a
  service loop, call the memory backend periodically, or publish periodic
  `memory_decay`.
- Automation retention service tick shipped in slice 190:
  `oran-automation::MemoryRetentionService` consumes a stored retention job,
  reuses the cadence planner, calls a supplied
  `memory::longterm::Backend::decay(...)` only when due, records
  success/failure run rows, and advances `last_fired_at` only after success.
  Backend failures are durably recorded and leave state unadvanced for explicit
  retry policy.
- Periodic retention hook publishing shipped in slice 191:
  `MemoryRetentionService` can publish advisory `memory_decay` metadata through
  a caller-supplied `hook::Bus` after successful due retention records its run
  and advances `last_fired_at`. Not-due ticks and backend failures publish
  nothing, advisory sink failures stay non-fatal, and bootstrap still does not
  open `automation.db` or run a background loop.
- Automation runtime state handle shipped in slice 192:
  `oran-automation::AutomationRuntime::open(...)` is the explicit caller-owned
  boundary for automation state. It creates parent directories, opens and
  migrates `automation.db`, keeps the `storage::Pool` plus
  `AutomationRepository` lifetime stable, exposes the migration report and
  repository, and can construct `MemoryRetentionService` over that state.
  Bootstrap still does not open `automation.db` or run a background loop.
- Automation retention loop step shipped in slice 193:
  `oran-automation::MemoryRetentionLoop::run_once(...)` ticks one stored
  retention job immediately, waits only within a caller-provided budget for the
  next scheduled fire, delegates due work back to `MemoryRetentionService`,
  reports cancellation while waiting, and rejects negative wait budgets. It is
  still a single explicit awaitable, not memory-owned periodic execution.
- Automation retention job lifecycle hooks shipped in slice 194:
  due `MemoryRetentionService::tick(...)` calls publish advisory
  `job_started`, `job_failed`, and `job_finished` metadata through the
  caller-supplied hook bus. Outcome events are emitted only after durable
  run/state transitions, not for not-due ticks, and the payload carries no
  decayed record content.
- Automation retention leases shipped in slice 195:
  `oran-automation::AutomationRepository` now stores retention job lease rows
  in `automation.db`, and `MemoryRetentionLoop::run_once(...)` leases only due
  tick execution. Planning and waiting do not hold a lease, so cancellation
  while waiting cannot strand one; active leases return conflicts and expired
  leases can be replaced by a new owner.
- Optional `MEMORY.md` mirror under `<workspace>/.orangutan/memory/`.
- Blocking `memory_read_before`, automation service-loop ownership, full
  scheduler/category lifecycle ownership, and memory-write rewrite/annotation
  remain downstream.

## Scope (v1.1)

- `Memory::team::Store` — shared tier for `oran-orchestration` teams.
- `kind = team` for cross-tier search visibility.
- Approval signing replay-safe across rotations.

## Scope (v2)

- Externalized/semantic embedding store via `oran-http::Client`; slice 178's
  local deterministic embedding owner is only the bootstrap plumbing owner.

## Out Of Scope

- Cross-runtime sync without explicit `scope_pin`.
- Memory replication.

## Acceptance Criteria

1. A `SessionStore::append(...)` followed by `SessionStore::load(...)` round-trips
   500 messages without loss. **Status:** closed for
   `memory::session::Store` by `test-memory` slice 130 coverage; slice 131 wires
   the runtime assembly owner, and slice 132 wires the bootstrap runner
   persistence path.
2. `longterm::Runtime::search("react agent loop", limit=10)` returns within 50 ms on
   a 10 k-record corpus. **Status:** closed for the shipped lexical, reference
   vector, and gated sqlite-vec search paths; slice 162 ships the runtime search
   seam over the default FTS5 lexical `Backend`, slice 163 gives configured
   bootstrap runs an owned `memory.db` backend/runtime, slice 164 lets
   prompt-runner callers opt into one prompt-boundary recall, slice 165 lets
   ordinary configured-route startup enable that recall through config, and
   slice 166 lets that config constrain recall to selected record kinds; slice
   167 adds the first selectable query-derivation policy, and slice 168 exposes
   read-only recall through the permissioned `memory.recall` tool. Slice 171
   adds the 10k-record FTS5 bench over `longterm::Runtime::search`, with a
   local result of **~15.08 ms / batch** for `limit=10`. Slice 172 adds
   the library-local `HybridRuntime` composition contract, and slice 173 adds the
   FTS5-vs-vector-vs-hybrid comparison bench over one shared 10k corpus (hybrid
   **~16.18 ms / batch**, also within 50 ms). Slice 176 adds the gated sqlite-vec
   vector backend, and slice 177 reports sqlite-vec vector-only
   **~3.03 ms / batch** plus FTS5+sqlite-vec hybrid **~18.96 ms / batch** on the
   same corpus. Slice 178 adds gated bootstrap embedding/vector ownership for
   configured-route hybrid recall and memory tools.
3. The MEMORY.md mirror, when enabled, reflects all kinds + records within 1 s of the
   underlying DB write.
4. Decay marks records older than `policy.forget_after_unused` as shadow; they no
   longer surface in default search. **Status:** partially closed at the
   backend boundary. Slice 181 closes the recall-side read metadata
   prerequisite: successful `Runtime::recall` and `HybridRuntime::recall`
   advance `last_read_at` through `Backend::touch`, while plain `search(...)`
   remains side-effect free. Slice 182 adds `Backend::decay(DecayRequest)` and
   the default FTS5 implementation that shadows scoped, visible records matching
   the unused-before and importance-floor policy inputs, then keeps them out of
   default search. Slice 183 adds the typed `memory.longterm.retention` config
   contract for those policy inputs. Slice 184 closes the configured-route
   startup owner by applying one bounded decay pass before prompt/tool reads.
   Slice 185 adds an assembly/banner diagnostic for whether that startup pass
   ran and how many records it shadowed. Slice 186 adds startup decay hook
   publishing through advisory `memory_decay` metadata and build-only startup
   hook bindings. Slice 187 adds the pure `oran-automation` retention cadence
   planner that can produce the same `DecayRequest` shape when a periodic job
   is due. Slice 188 maps configured-route retention into an automation-owned
   job descriptor and stores it on `RuntimeAssembly` for future scheduler
   ownership. Slice 189 persists that descriptor's job state and future run
   history through `AutomationRepository`. Slice 190 adds a caller-driven
   `MemoryRetentionService::tick(...)` execution boundary that runs due decay
   through a supplied backend and records outcomes. Slice 191 publishes
   periodic advisory `memory_decay` metadata from that explicit tick owner when
   a caller supplies a hook bus. Slice 192 adds the explicit caller-owned
   `AutomationRuntime::open(...)` state handle for automation DB opening,
   migration, and repository/service lifetime ownership. Slice 193 adds the
   caller-started retention loop step that can wait within a caller budget and
   propagate cancellation while waiting. Slice 194 adds advisory
   `job_started`, `job_failed`, and `job_finished` metadata from due retention
   ticks after the corresponding durable state boundary. Slice 195 adds stored
   retention job leases and loop-side due-run lease ownership.
   Bootstrap service ownership and long-running scheduler ownership remain
   open.
5. A `memory.write.before` hook can veto a write. **Status:** closed for the
   bootstrap `memory.remember` tool path in slice 179. `AgentPromptRunner`
   publishes blocking `memory_write_before` after parsing/scoping the record and
   before calling the long-term lexical/vector backends; a veto returns
   `ErrorKind::permission_denied` with `event=memory_write_before`,
   `reason=blocked_by_hook`, `decision_kind`, and `hook_reason`, and the record is
   not persisted. `rewrite` and `require_approval` decisions remain unsupported
   for this memory-write consumer and are rejected as blocked hook decisions.
6. `tests/memory/` >= 85% coverage. **Status:** default `test-memory` currently
   reports 38 cases / 841 assertions, including long-term contract validation,
   fake async backend interface coverage, public `Fts5Backend` migration /
   scoped search / filtering / update / touch / decay / delete coverage, and
   `longterm::Runtime` validation / deterministic recall-framing plus
   recall/remember/forget `data_json` coverage. Slice 178 adds deterministic
   local text/record embedding helper coverage. Slice 172 adds hybrid-runtime validation,
   lexical/vector merge ordering, vector-only hydration, stale vector-row skip,
   and recall-framing coverage. Slice 181 adds default-build coverage for
   runtime recall touches, hybrid recall touches, and FTS5 monotonic
   `last_read_at` updates that do not rebuild indexed text. Slice 182 adds
   default-build FTS5 decay coverage for scoped candidate selection,
   importance floors, already-shadow rows, batch limits, and default
   search-hidden / include-shadow-visible behavior. Slice 183 adds `oran-config`
   retention-policy coverage for defaults, explicit values, malformed values,
   and strict/loose unknown retention fields. Slice 184 adds bootstrap coverage
   for configured-route startup retention consumption plus assembly-level
   startup decay before the long-lived memory backend is exposed. Slice 185
   tightens bootstrap assertions for the startup decay shadow-count diagnostic.
   Slice 186 adds bootstrap and hook coverage for startup `MemoryDecayPayload`
   delivery, startup hook binding validation, and startup-only unbind behavior.
   Slice 195 adds `test-automation` coverage for retention lease migration,
   active/expired/release/reacquire semantics, due-loop conflicts, and
   cancellation-while-waiting without held leases; `test-automation` reports
   30 cases / 390 assertions.
   Slice 176 adds
   gated sqlite-vec disabled-build coverage, plus `--vector_memory=y` coverage
   for scoped upsert/search/remove and dimension-mismatch migration rejection.
   Gated `--vector_memory=y` `test-memory` remains last reported at 36 cases /
   791 assertions.
   `test-config` reports
   48 cases / 429 assertions for the recall, hybrid-search, and retention policy parsers, `test-hook`
   reports 37 cases / 299 assertions for hook payload and redaction coverage, and `test-bootstrap`
   reports 124 cases / 1054 assertions by default and 120 cases / 958 assertions
   with `--vector_memory=y` for the assembly, prompt-runner, and
   configured-route recall consumers, including slice-167 query-strategy
   coverage, slice-168 `memory.recall` tool binding, and slice-169
   `memory.remember` plus slice-170 `memory.forget` tool bindings and
   slice-175 hybrid-search fail-fast coverage. Slice 178 adds gated coverage for
   vector-memory assembly ownership, configured-route hybrid startup, hybrid
   prompt-boundary recall, hybrid `memory.recall`, and vector mirroring for
   `memory.remember`; slice 179 adds default-build bootstrap coverage for
   `memory_write_before` / `memory_write_after` / `memory_forget` publishing and
   veto/no-persist behavior, and slice 180 adds default-build bootstrap coverage
   for `memory_read_after` publishing after prompt-boundary recall and
   `memory.recall`.
7. `bench/memory/search-fts5-vs-vector` (v2): reports the FTS5 baseline + vector
   results in machine-readable JSON. **Status:** partially open; slice 171 adds
   the FTS5 baseline scenario under `bench/memory/scenarios/longterm_fts5.cpp`,
   slice 172 adds the hybrid composition contract, and slice 173 adds
   `bench/memory/scenarios/search_fts5_vs_vector.cpp`, which compares FTS5-only
   `Runtime::search`, an in-bench brute-force cosine `VectorBackend`, and
   `HybridRuntime::search` over one shared 10k corpus. Slice 177 adds the gated
   sqlite-vec vector-only and FTS5+sqlite-vec hybrid rows under
   `--vector_memory=y`. The remaining gap is the unified machine-readable JSON
   emission described by spec 0010.

## Design Doc Cross-References

- [`../design-docs/memory-system.md`](../design-docs/memory-system.md)
- [`../design-docs/secrets-and-state.md`](../design-docs/secrets-and-state.md)
- [`../design-docs/team-collaboration.md`](../design-docs/team-collaboration.md)
  (shared tier)

## Risks

- WAL contention if `sessions.db` and `memory.db` are not split — they are. Verify
  with a bench under multi-conversation load.
- FTS5 ranking quality on Chinese / Japanese / Korean text — track in the
  tech-debt-tracker; consider `simdjson`-style tokenizer or vector for non-Latin
  corpora in v2.

## Validation

```sh
xmake build oran-memory
xmake run test-memory
xmake build test-automation && xmake run test-automation
xmake run test-bootstrap
scripts/bench-compare.sh memory
```

Slice 195 reports `test-automation` at 30 cases / 390 assertions for the
retention planner, repository, service tick, periodic hook-publish,
caller-owned runtime state-handle, caller-started loop-step, retention job
lifecycle hook boundaries, and retained job lease ownership.

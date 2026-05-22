# 0012 — Tool Scheduler + Bounded Runtime State

## User Problem

When `oran-agent` lands and providers start returning multiple `tool_use`
blocks per turn, the agent has to decide *how* to run them. Today the
registry dispatches one call at a time on the agent strand; the provider
layer has no logic for consuming parallel tool-use responses. Once an LLM
emits "read these four files in parallel" the runtime either fans out
unsafely (race on a path lock that doesn't exist) or serialises everything
(prompt round-trip dominated by IO that could have run concurrently).

Worse, the same agent loop accumulates small caches and grants over a
long-running process: path locks, deferred-tool promotions, rendered tool
blocks, compiled regexes, approval grants, route-health state. Any of these
that grows without a bound is a leak the next slice has to chase.

This spec defines the v1 parallelism contract — what runs concurrently,
what serialises, how cancellation propagates, how results are returned —
and the bounded-state contract — every cache-like structure has an explicit
TTL/LRU and is observable. Both contracts pre-date `oran-agent` itself;
they live in `oran-tool` and `oran-async` so that the first agent loop
inherits them rather than re-inventing them.

Today's seams that motivate this spec:

- `tool::Registry::dispatch` handles one call at a time (`src/oran-tool/
  registry.cpp:182-184`). No batching, no scheduler, no per-path lock.
- `oran-agent` does not exist yet (`docs/STATUS.md`), so the scheduler can
  land *before* the loop and dictate the loop's tool-call contract rather
  than retrofit one.
- `permission::ApprovalBroker` already reaps expired grants
  (`reap_expired(now)`) but never had a "max grants per identity" ceiling.
- Hook bus dispatch is advisory-only with sequential fan-out
  (`src/oran-hook/bus.cpp:54-81`); multi-sink provider/token-stream events
  will surface this once parallel tools land.

## Scope (v1)

The MVP delivers the *correctness* primitives — schedule, lock, bound,
observe — so the future agent loop can call them safely on day one.

- **`agent::ToolScheduler`** (lives in `oran-agent` once the lib exists;
  pre-`oran-agent` lives in `oran-tool::Scheduler` and migrates in the
  same slice that creates `oran-agent`):
  ```cpp
  class ToolScheduler {
   public:
    struct Options {
      std::size_t  max_parallel_tools{4};        // config-backed
      Duration     per_call_timeout{60s};
      Duration     idle_lock_ttl{5min};
    };

    Awaitable<std::vector<DispatchResult>>
    run_batch(std::vector<ToolUse> batch, DispatchContext ctx);
  };
  ```
- **Per-path lock table** for mutating tools (`file.write`, `file.edit`,
  `file.modify`, `file.delete`):
  - Lock key: canonical workspace-resolved path (spec 0013).
  - Lock type: exclusive for write/edit/delete, shared for read.
  - Lock rows are bounded with an idle TTL (default 5 min) and reaped on
    a background tick. A cancelled mutation never leaves a dead row.
- **Bounded parallelism**:
- **Read-only tools** (may run concurrently up to
  `max_parallel_tools`): `file.read`, `file.search`,
  `directory.list`, `directory.scan` (future), `code.outline`
  (future), `code.symbols` (future), `code.references` (future),
  `memory.recall` (future).
- **Mutating tools** (serialise per canonical resource):
  `file.write`, `file.edit`, `file.modify` (future), `file.delete`,
  `memory.remember` (future), `memory.forget` (future).
- **Globally serialised tools** (compete for a process-wide slot
  until they ship a per-resource lock):
  `shell.exec` (future — workspace lock until it gains per-cwd
  scoping), `agent.spawn` (future), `tool.runtime_loader` (future).
  Their `ToolDef::required_capabilities` already implies the slot
  via `spawn_subprocess` / `runtime_loader`.

The classification is derived from `ToolDef::required_capabilities`
(no per-call override; the operator cannot accidentally promote a
mutating tool to read-only). New built-ins declare their lock class
implicitly via the capability list.
- **Approval-gated calls resolve before execution.** A `Verdict::ask`
  short-circuits the batch slot until the broker call returns; a denied or
  pending high-risk call must not be hidden behind unrelated successful
  calls in the agent transcript.
- **Ordered results.** `run_batch` returns results in the original
  `tool_use` order even when execution finishes out of order. Determinism
  beats the milliseconds gained by arbitrary ordering — the prompt-cache
  contract in [`../rules/prompt-design.md`](../rules/prompt-design.md)
  depends on stable byte order.
- **Per-call invariants preserved.** Every call still gets:
  - one permission decision,
  - one `permission::AuditEvent`,
  - the full `tool_before` / `tool_dispatched` / `tool_after` /
    `tool_error` hook lifecycle,
  - a cancellation slot derived from the batch's parent slot,
  - the configured `per_call_timeout` enforced by the scheduler, not the
    tool handler.
- **`tool::Registry` stays single-threaded.** Do not add internal locks to
  the registry as a first move. The scheduler owns concurrency at the
  call-graph boundary; the registry runs on the agent strand and the
  scheduler hops to worker executors at dispatch time.
- **`BoundedCache<Key, Value>`** generic primitive (also referenced by
  spec 0011). **Status (slice 44, 2026-05-22):** shipped in `oran-core`
  as `core::BoundedCache<Key, Value, ByteSizeOf = BoundedCacheNoByteBudget>`
  (`<oran/core/bounded_cache.hpp>`). The single-strand contract, the LRU
  + insert-based TTL + byte-budget eviction order, and the `Stats`
  accessor are all live. The shipped API returns a non-owning
  `Value*` from `get` rather than the `std::optional<Value>` the spec
  sketches below — `std::optional<unique_ptr<re2::RE2>>` cannot be
  populated from a moved-in source on a re-hit, and the spec's
  intended consumers include move-only types. Existing call sites
  pass an explicit `core::Time` `now` everywhere so the cache stays
  clock-agnostic (testable without a real clock; cooperates with
  `core::time::now_utc` in production code).
  ```cpp
  template <class Key, class Value>
  class BoundedCache {
   public:
    struct Options {
      std::size_t            max_entries;
      std::size_t            max_bytes{0};   // 0 = no byte cap
      std::chrono::seconds   ttl{0s};        // 0s = no TTL
    };
    struct Stats {
      std::uint64_t hits;
      std::uint64_t misses;
      std::uint64_t evictions_lru;
      std::uint64_t evictions_ttl;
      std::uint64_t evictions_bytes;
      std::uint64_t rejected_oversize;     // slice 44 addition
      std::size_t   current_entries;
      std::size_t   current_bytes;
    };

    Value*               get(const Key&, core::Time now);   // shipped
    void                 put(Key, Value, core::Time now);
    std::size_t          reap(core::Time now);
    const Stats&         stats() const noexcept;
  };
  ```
- **First bounded-state inventory** (each gets an explicit policy in v1):
  | Structure | Policy | Default |
  | --- | --- | --- |
  | Path-lock table | reap idle rows on TTL | 5 min |
  | Deferred-tool promotion set (per session) | LRU on session size | 16 entries |
  | Approval broker grants | TTL + max-per-identity | existing TTL + 64 per identity |
  | Provider route health / retry backoff | TTL per route | 30 s |
  | Regex compile cache (`file.search`) | LRU | 64 entries |
  | Tool catalog rendered-block cache | LRU by `(ToolDef hash, cache_version)` | 256 entries |
- **Observable.** Every cache and lock table exposes a `Stats` accessor;
  once `oran-log` lands, a periodic tick publishes them as structured log
  events. Pre-`oran-log`, `--explain-rules`-style debug surfaces in
  `oran-bootstrap` expose the same numbers.

## Scope (v1.1)

- **Index caches** (built on `BoundedCache`):
  - Directory tree index (path, kind, size, mtime, hidden flag, ignored
    flag) — first cache because it benefits both `directory.scan` and
    `file.search`.
  - Tool schema index — mapping name/category/capability to `ToolDef` and
    rendered prompt block hash.
  - Skill catalog index — frontmatter metadata + body hash for hot-reload
    and prompt rendering (cross-refs spec 0009).
  - Symbol/reference index (LSP-backed) — keyed by canonical path +
    language-server version + content hash.
- **Persisted indexes** land under `<workspace>/.orangutan/cache/indexes/`
  with the version header from spec 0011 v2 (schema version, build slice,
  workspace root canonical path, config hash, fingerprint strategy).
- **Singleflight on dispatch**: N concurrent `run_batch` calls that
  request the *same* `(tool_name, input_hash)` collapse into one
  in-flight execution; the rest await its result. Useful for memory
  recall and `file.read` of hot paths.
- **`publish_blocking` consumption.** The scheduler is the first consumer
  of `hook::Bus::publish_blocking` (tracked in the tracker's 2026-05-18
  row) for `permission_ask_rendered` rendering. The advisory hook bus
  stays for fire-and-forget sinks.

## Scope (v2)

- **Cost-aware scheduling.** When `ProviderRoute` cost metadata lands
  (cross-ref [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)
  "Provider Cost Awareness"), the scheduler can preempt a low-priority
  batch when a budget threshold trips.
- **Inter-agent fairness.** When orchestration (spec 0004) spawns workers
  that share the process, the scheduler ranks ready batches by agent
  identity + priority. Today's single-agent process gets FIFO.
- **CPU-class executors.** Move scheduler hops to a dedicated worker pool
  separate from the agent strand and the IO blocking pool, once
  `oran-async` ships the third executor (cross-ref
  [`../design-docs/async-model.md`](../design-docs/async-model.md)).

## Out Of Scope

- A general-purpose work-stealing scheduler. The agent's call graph is
  shallow; FIFO + bounded parallelism is enough for v1/v1.1.
- Cross-process coordination (two `orangutan` processes scheduling against
  the same workspace). The agent platform vision pins this as stretch.
- Distributed cache replication. Bounded state stays per-process per
  [`../design-docs/agent-platform.md`](../design-docs/agent-platform.md)
  "Anti-Goals".

## Acceptance Criteria

1. **Bounded parallelism.** A batch of 10 fake-tool calls with
   `max_parallel_tools=4` and per-call latency 50 ms completes in
   ≥ 150 ms and ≤ 250 ms; never runs more than 4 concurrently. Pinned via
   a fake-tool fixture that records its entry/exit timestamps.
2. **Ordered results.** A batch whose call N takes longer than N+1 still
   returns results in the order `[result_0, result_1, …, result_N]`.
   The agent's transcript bytes are byte-identical regardless of execution
   ordering.
3. **Per-path serialisation.** Two concurrent `file.write` calls to the
   same canonical path execute strictly one after the other; two writes
   to *different* paths run in parallel. Pinned via a fake mutating tool
   that records its lock acquisition timestamps.
4. **Read/write coexistence.** A read on path P concurrent with a write
   on path P obeys the read-write lock: reads complete before the write
   starts, *or* the read sees a fingerprint that matches the post-write
   state. Never a stale-after-write read.
5. **Cancellation propagation.** Cancelling the parent token aborts every
   in-flight call within `100 ms` (modulo any tool whose handler does not
   poll its cancellation slot — that's a tool bug, audited as
   `error_kind=cancellation_lag` once `oran-log` lands).
6. **Timeout enforcement.** A tool that exceeds `per_call_timeout` returns
   `Error::cancelled` with `reason=timeout`; the audit row records
   `cancelled` and the hook lifecycle emits `tool_after` with
   `succeeded=false`.
7. **Per-call audit + hook coverage.** A batch of N calls produces exactly
   N audit rows and exactly N `tool_after` hook publishes (failure paths
   still emit `tool_after`), regardless of execution order.
8. **`BoundedCache` invariants.** Capacity is never exceeded by more than
   one entry transiently (the put-then-evict pattern). TTL eviction fires
   on `reap(now)` at most once per entry. Byte budget rejects no inserts
   but evicts the largest oldest entries until budget holds.
9. **Stats observability.** `BoundedCache::stats()` returns
   monotonically increasing hit/miss/eviction counts; `current_entries`
   and `current_bytes` reflect post-reap state.
10. **Bounded path-lock table.** A workflow that touches 10 000 distinct
    paths and finishes leaves the lock table at ≤ 100 rows after one
    `reap` tick at `idle_lock_ttl + 1s`.
11. **`tests/tool/`** (or `tests/agent/` once the lib exists) ≥ 90% on the
    scheduler matrix (parallelism × ordering × lock kind × cancellation
    × timeout × approval gate).
12. **`bench/tool/scheduler_overhead`** reports dispatch overhead under
    bounded parallelism ≤ 1.5× the single-call dispatch overhead (spec
    0002's ≤ 50 µs ceiling).

## Design Doc Cross-References

- [`../design-docs/tool-runtime.md`](../design-docs/tool-runtime.md) —
  the registry stays single-threaded; the scheduler sits between provider
  and registry. This spec is the v1 home for the scheduler contract; the
  design doc gains a "Scheduler Boundary" section pointing back here.
- [`../design-docs/async-model.md`](../design-docs/async-model.md) — the
  scheduler hops to the agent's executor; the bounded-state primitives
  use `core::Time` (steady clock) for TTL reaping so they are unaffected
  by wall-clock skew.
- [`../design-docs/permissions-and-hooks.md`](../design-docs/permissions-and-hooks.md)
  — `publish_blocking` consumption is the scheduler's, not each tool's.
- [`0011-file-view-and-caching.md`](0011-file-view-and-caching.md) —
  consumes the per-path lock for synchronous cache invalidation; consumes
  `BoundedCache` for the line-offset index and regex cache.
- [`0013-workspace-and-path-policy.md`](0013-workspace-and-path-policy.md)
  — lock keys are *resolved* canonical paths, not input strings.
- [`0014-structured-tool-output.md`](0014-structured-tool-output.md) —
  the scheduler enforces output byte caps and aggregates
  `ToolUsage` across parallel calls.
- [`0015-blocking-hook-decisions.md`](0015-blocking-hook-decisions.md)
  — the scheduler is the first consumer of `publish_blocking`;
  per-call timeout enforcement covers the blocking-hook timeout too.
- [`0017-fake-provider-first-agent-loop.md`](0017-fake-provider-first-agent-loop.md)
  — scheduler tests piggyback on the fake-provider harness: a
  fake provider emits a multi-`tool_use` response, the scheduler
  fans out, ordering and lock-table invariants are pinned without
  network.
- [`0018-first-loop-observability.md`](0018-first-loop-observability.md)
  — scheduler stats (per-batch parallelism, per-path lock waits,
  cache hit/miss) ride in the per-turn trace row's `context_json`
  until they graduate to typed columns.
- [`0008-permissions.md`](0008-permissions.md) — approval grants gain a
  per-identity ceiling that lives in the broker.

## Risks

- **Premature distribution.** A scheduler is the right place to add
  parallelism, but adding it before `oran-agent` ships risks an unused
  abstraction. Mitigation: ship v1 in `oran-tool::Scheduler` with a single
  fake-tool test bucket; migrate into `oran-agent` in the same slice that
  creates the lib, with no behaviour change.
- **Lock-table memory leak.** A pathological agent touches a fresh path
  per tool call; the lock table grows. Mitigation: TTL reap + the
  acceptance criterion above.
- **Cancellation lag in handlers.** Tools whose handlers do not poll the
  cancellation slot block the scheduler's cancellation guarantee.
  Mitigation: the per-call timeout fires regardless; the
  `cancellation_lag` audit record names the offending tool.
- **`BoundedCache` over-generalisation.** A generic template before a
  second call site exists invites bike-shedding. Mitigation: the v1
  cache lives in whichever lib first needs it (`oran-tool` for the regex
  cache, or `oran-io` for the line-offset index); the lift to `oran-core`
  is its own slice with two demonstrated call sites.
- **Audit fan-out on batches.** N parallel calls means N AuditSink::record
  calls in flight. The current `StorageAuditSink` writes through the
  `Pool`'s writer; bench `bench-oran-tool/scheduler_audit_fanout` must
  show that a batch of 8 calls does not starve the pool's writer.

## Validation

```sh
xmake build oran-tool          # pre-oran-agent home
xmake test test-tool           # scheduler + bounded-cache + lock-table
xmake build bench-oran-tool
xmake run bench-oran-tool scheduler_overhead
xmake run bench-oran-tool scheduler_audit_fanout
xmake run orangutan -- --explain-rules  # bounded-state stats once wired
```

## Out-of-Band Cross-Cuts

- `docs/design-docs/tool-runtime.md` gains a "Scheduler Boundary"
  subsection clarifying that the registry is single-threaded by design;
  the scheduler owns parallelism.
- `docs/design-docs/agent-platform.md` "Cross-Cutting Concerns" — the
  `Backpressure` and `Cancellation` bullets gain the scheduler as their
  first concrete enforcement point.
- `docs/exec-plans/tech-debt-tracker.md` — the deep-review §Parallel tool
  calls and §Bounded runtime state rows retire as this spec's slices
  land.
- `docs/rules/async-and-concurrency.md` — the rule against
  `std::thread` is reinforced by pointing at the scheduler as the
  *only* place that schedules off the agent strand.

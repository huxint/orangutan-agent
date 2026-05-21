# Orangutan v2 — Deep Architecture & Implementation Review

> **⚠️ STALE — historical artifact (slice 30 snapshot).**
>
> This review was generated against slice 30. Slices 31–33 have since
> closed the four rank-0 items it identified (channel race + executor,
> bus exception, doc drift, atomic write footgun, file.search
> cancellation polling). Several other findings have moved into
> follow-up specs.
>
> **For the *current* roadmap, do not act on §6 here — use the live
> sources of truth instead:**
>
> - **Project state today:** [`docs/STATUS.md`](docs/STATUS.md).
> - **Remaining cleanup from this review:**
>   [`docs/exec-plans/tech-debt-tracker.md`](docs/exec-plans/tech-debt-tracker.md)
>   `deep-review-2026-05-21` row (file:line targets preserved).
> - **Future-feature roadmap (workspace policy, file-view system,
>   parallel tool scheduler, bounded state, structured tool output,
>   prompt cache, blocking hooks, fake-provider-first loop,
>   observability):**
>   [`docs/product-specs/0011`](docs/product-specs/0011-file-view-and-caching.md)–
>   [`0018`](docs/product-specs/0018-first-loop-observability.md).
>
> A second deep review on 2026-05-21 produced
> `/tmp/orangutan-refactor-agent-tool-review-2026-05-21.md` and a
> companion agent-loop-foundation note
> `/tmp/orangutan-agent-loop-foundation-2026-05-21.md`. Their
> recommendations have been *fully absorbed* into specs 0011–0018 + the
> tracker row above; the temp files themselves are not authoritative
> and may be removed.
>
> The body below is preserved for historical context and because the
> tracker row cross-references its `§4.x.y` anchors. Reading it cold to
> plan new work risks repeating closed slices.

---

**Generated:** 2026-05-21 · **Reviewer:** Claude Opus 4.7
**Scope:** Full project goals → current state → defects → forward-looking agent / tool design
**Read-time:** ~25 minutes · **Action items:** §6 (prioritized — see banner above; many already closed)

---

## 1. Executive Summary

Orangutan v2 is a **C++26 ground-up rewrite** of a legacy ~40 kLoC agent runtime
(`orangutan/`). The goals are not "another LLM chatbot" — they are explicit
**platform** goals: a single binary that hosts N agents across M interfaces
(CLI / web / channel / cron) on a uniform asio-coroutine foundation, with
permission / hook / audit as first-class subsystems rather than bolt-ons.

**Where the project is at slice 30 (~11.6 kLoC):**

- **Foundation libraries are mature** — `oran-core / async / io / storage / config /
  permission / hook / tool / cli / bootstrap` all have tests + benches, and they
  compose cleanly. The compile-budget discipline appears to be working: 11.6 kLoC
  in 10 libs with deep public-header forward-decl hygiene.
- **The agent itself doesn't exist yet.** `oran-agent`, `oran-provider`,
  `oran-prompt`, `oran-memory`, `oran-skill`, `oran-orchestration`,
  `oran-automation`, `oran-channel*`, `oran-web`, `oran-log`, `oran-http` —
  all planned, none coded. The platform is ~30% built; the agent surface is 0%.
- **Quality of what exists is high.** Permission/audit/hook integration in tool
  dispatch is well-thought-out, with HMAC-signed approvals + replay tracking +
  capability-aware gating. The legacy audit's friction list has been
  systematically addressed in the design.
- **The riskiest part of the project is still ahead.** The legacy code's
  failure mode was *compile-time*, not correctness. The legacy code shipped a
  ReAct loop, providers, channels — what it failed at was *evolving* them.
  v2's bet is that modular boundaries + permission/hook seams + bench parity
  will keep iteration speed up as features land. That bet hasn't been tested
  yet because the iteration-heavy code (agent loop, provider adapters,
  channel adapters) is the un-built half.

**Top-line judgment:** the architecture is sound, the foundation is solid, the
real test is whether the agent loop slice can land without violating any of the
cross-cutting invariants (cancellation, permission gating, hook coverage,
prompt-cache discipline). The biggest open risk is **prompt-cache discipline
under iteration pressure** — see §4.6.

---

## 2. Project Understanding

### 2.1 What is being built

A **runtime for agents**, deliberately not framed as a chatbot. The vision
(`docs/design-docs/agent-platform.md`) treats each agent as a *typed
configuration* of:

- A base model + protocol (Anthropic Messages / OpenAI Responses, with fallback)
- An identity scope (keys that namespace memory & audit)
- A subset of the global tool registry (with per-tool permission overrides)
- A memory profile (which tiers visible)
- A skill set (markdown skills under `<workspace>/.orangutan/skills/`)
- Hook bindings (which sinks subscribe to which lifecycle events)
- A mailbox address (`<team>/<agent-name>`)

The runtime hosts **many** agents in one process and exposes them through
**many** interfaces (CLI REPL / single-shot, web HTTP+SSE, channel adapters
QQ/Discord/Slack/Telegram/Webhook, automation cron/periodic/triggered,
orchestration mailboxes).

### 2.2 Why a rewrite at all

`docs/references/orangutan-legacy-audit.md` enumerates 15 friction points in
the legacy code; the worst three:

1. **~70 s compile time** with no PCH/LTO/modules/unity — every iteration
   penalty.
2. **`stdexec` (NVIDIA gtc-2026 fork) bled across 5+ libraries** — a
   compile-time + toolchain coupling tax.
3. **Mixed expected-vs-throwing SQLite API** with ~120 callsites — too
   expensive to refactor in place.

v2's response: pin C++26 / GCC 16.1; standardize on asio + coroutines; ship
`oran-storage` expected-only from day one; enforce module/include hygiene via
mechanical scripts (`scripts/check-*.sh`).

### 2.3 Layered architecture

Three layers, one-way deps (enforced by `check-deps.sh` planned):

```
INTERFACE  →  CLI · Web · Channel · Cron
AGENT      →  ReAct loop · Tool registry · Provider · Permissions
              Memory · Hook bus · Orchestration
PLATFORM   →  storage · config · secrets · http/ws · process · io
              asio executor · logging
```

The platform layer never reaches up. Every effectful action goes through
`oran-permission`. Every observable lifecycle point publishes through
`oran-hook`. Cancellation is universal.

### 2.4 The six cross-cutting concerns

Documented in `agent-platform.md` — **every subsystem must address them
uniformly**:

1. Observability (structured events with causal IDs)
2. Cancellation (asio cancellation_slot, per-subsystem semantics)
3. Backpressure (bounded queues default; unbounded justified)
4. Identity (`agent_key` + `runtime_key`, scoped memory/audit)
5. Permissions (single rule engine, capability gating)
6. Hooks (enumerable lifecycle events, pluggable sinks)

This consistency is what should make v2 *evolvable* where the legacy was not.

---

## 3. Current Implementation State (slice 30)

### 3.1 Library inventory & status

| Library | Lines | Status | Tests |
|---|---|---|---|
| `oran-core` | small | ✅ mature | 54 cases / 370 asserts |
| `oran-async` | small | ✅ MVP (Runtime + Channel + sleep) | 8 / 38 |
| `oran-io` | ~500 | ✅ file/dir/delete MVP | 13 / 51 |
| `oran-storage` | ~1000 | ✅ Pool + caches + Session/Audit repos + `#embed` migrations | 60 / 706 |
| `oran-config` | mid | ✅ typed runtime + permissions + agents overlay | 19 / 148 |
| `oran-permission` | mid | ✅ rules + ask flow + HMAC tokens + broker + replay + audit pipeline | 83 / 379 |
| `oran-hook` | small | ✅ Bus advisory; blocking-veto deferred | 14 / 79 |
| `oran-tool` | mid | ✅ registry + 6 file built-ins + audit/hook integration | 89 / 712 |
| `oran-cli` | small | ✅ pre-agent shell scaffold | 5 / 30 |
| `oran-bootstrap` | mid | ✅ RuntimeAssembly + SignalScope + --audit-init + --explain-rules | 44 / 140 |
| `oran-log` | — | ❌ not started |  |
| `oran-http` | — | ❌ not started |  |
| `oran-provider` | — | ❌ not started |  |
| `oran-prompt` | — | ❌ not started |  |
| `oran-memory` | — | ❌ not started |  |
| `oran-skill` | — | ❌ not started |  |
| `oran-agent` | — | ❌ not started |  |
| `oran-orchestration` | — | ❌ not started |  |
| `oran-automation` | — | ❌ not started |  |
| `oran-channel*` | — | ❌ not started |  |
| `oran-web` | — | ❌ not started |  |

### 3.2 Tool catalog (the agent's hand)

Six file tools shipped, all gated by capabilities:

| Tool | Capability | Notes |
|---|---|---|
| `file.read` | `read_file` | Default 16 MiB cap, no line numbers (see §4.1) |
| `file.write` | `write_file` | `truncate`/`append`/`fail_if_exists`, optional `create_parents` |
| `file.edit` | `edit_file` | `old_string` + `new_string` + optional `replace_all` |
| `file.search` | `read_file` | Literal substring or re2 regex (slice 24); dotfile skip; binary heuristic |
| `directory.list` | `list_directory` | Single-level; sorted by path; `no entries` literal for empty |
| `file.delete` | `delete_path` | Regular files only; refuses dirs+symlinks |

The dispatch pipeline (`src/oran-tool/registry.cpp`) wires audit + hook + broker
+ permission into a single coherent flow — see §4.3 for the detailed walk-through.

### 3.3 Permission system (the agent's leash)

- 4 modes (`strict / default / permissive / sandboxed`) with `Defaults::for_mode`
- 3-layer rule merge (defaults + global config + per-agent overlay)
- `*`-glob tool matching, optional re2 `InputPattern` per rule
- Capability-aware gating (`Rule::capability` + tool's `required_capabilities`)
- HMAC-signed approval tokens (libsodium, per-process key, rotated per restart)
- `ApprovalBroker` tracks replay state, returns typed rejection reasons
- `StorageAuditSink` writes every decision to `audit.db` with SHA-256 input hash

This is one of the strongest parts of the codebase — it's complete, well-documented,
and integrated end-to-end with tool dispatch.

### 3.4 Hook bus (the agent's nervous system)

- 38-event enum covering agent / provider / tool / memory / channel /
  orchestration / automation / session / permission lifecycles
- `Bus::publish_advisory` iterates sinks in subscription order, never aborts
  on sink error, returns `PublishOutcome` with per-sink result
- `InProcessSink` is the only concrete sink yet; `ShellSink` / `WebhookSink` /
  `LuaSink` planned
- Tool dispatch publishes `tool_before / tool_dispatched / tool_error /
  tool_after` at the right exit points (audit-correct timing — see §4.3)

**Missing:** blocking publish with veto. Deferred until the first blocking
consumer (`permission_ask_rendered` operator prompt) needs it. This is tracked
in tech-debt; the decision to defer is defensible.

### 3.5 Storage (the agent's memory)

- SQLite WAL, one writer + reader pool per DB
- `Pool` with per-slot `StatementCache` (mutex-protected; FIFO waiter queue)
- `SessionRepository` (typed `core::Role` boundary)
- `AuditRepository` (with hex-encoded input_hash, typed outcome enum)
- Migrations: per-DB numbered `.sql` files, plus `#embed`-packaged
  built-ins so `bootstrap::run` runs migrations from inside the binary

**Missing:** `memory.db`, `automation.db`, vector backend, decay policy
runner, MEMORY.md mirror.

---

## 4. Findings — Bugs, Smells, Performance, Gaps

Findings labeled:
- **BUG** — definite correctness problem
- **SMELL** — likely problem, needs further check
- **PERF** — measurable perf issue
- **GAP** — designed-for, not yet present
- **STYLE** — convention drift, no functional impact

### 4.1 Tool layer — concrete defects

**BUG-4.1.1 — `file.edit` lacks atomic write.** The handler composes
`io::read_text_file` then `io::write_text_file` (`src/oran-tool/file_edit.cpp:122-150`).
If the write fails mid-stream, the file is left truncated or partially written —
a data-loss footgun for a tool an LLM will use frequently. **Fix:** write to
`<path>.orangutan.tmp` then `std::filesystem::rename` (atomic on POSIX).

**SMELL-4.1.2 — `file.edit` TOCTOU between read and write.** A racing process
between the two coroutine hops can be silently overwritten. Low-likelihood on
a single-user dev machine, but worth a stat-and-mtime-check before overwrite,
or a file-level lock via `flock(2)` while reading + writing. Documented as
"MVP" in the file header but not in any tech-debt row.

**SMELL-4.1.3 — `file.search` does not poll cancellation in the inner walk
loop.** The handler checks `cancellation_state` at the start and after the
`post` hop (`file_search.cpp:366-373`), but `walk_and_scan` is fully
synchronous after that. A pathological `pattern=""` over a multi-GB tree is
uncancellable. **Fix:** thread a `cancellation_state` reference into
`walk_and_scan` and check every N entries.

**SMELL-4.1.4 — `file.search` is the only tool that uses `<fstream>` directly
instead of `oran-io`.** It duplicates the `read_text_capped` logic + the
executor-hop discipline that `oran-io::run_blocking` already encodes. The
duplication exists because the io layer's `read_text_file` itself does a `post`
hop, which would be a double-hop inside a directory walk. **Fix:** expose
`io::run_blocking` as a public utility, and add an `io::read_text_no_hop`
variant for callers already on the executor thread.

**SMELL-4.1.5 — `file.search` root-path follows symlinks, nested-path skips them.**
The root is checked via `std::filesystem::is_regular_file` /
`is_directory` (which follow symlinks). Nested entries are explicitly skipped
when `entry.is_symlink(...)` returns true. This is asymmetric and inconsistent
with `file.delete`'s policy. **Fix:** unify on `symlink_status` everywhere.

**BUG-4.1.6 — `file.write` has no upper bound on `content.size()`.** An LLM
could request a multi-gigabyte string. The io layer also doesn't cap writes.
At minimum, mirror `ReadTextOptions::max_bytes` (16 MiB default) on the write
side.

**SMELL-4.1.7 — `file.edit` has no upper bound on replacement explosion.** A
`replace_all` with a long `new_string` and many matches could OOM.

**STYLE-4.1.8 — Copy-pasted JSON parse prelude.** All 6 handlers contain
near-identical try/catch + object-check + per-field type-check sequences
(~20 lines each). Worse: the `catch (parse_error)` + `catch (std::exception)`
sequence is redundant because `nlohmann::json::parse_error` derives from
`std::exception`. **Fix:** a `parse_input<T>(input_json, tool_name)` helper
that takes a typed struct + lambda field-extractor. Cuts ~120 LoC and removes a
class of future copy-paste bugs (forgetting to validate one field).

**STYLE-4.1.9 — Tool output convention drift.** `file.write` uses
`std::format("wrote {} bytes to {}", ...)`; `file.delete` uses `"deleted " + path`
(string concatenation); `file.edit` uses `std::format` but mixes
`parsed["path"].get<std::string>()` *after* the original `path` was moved into
the io call (`file_edit.cpp:154`). Per-field validation messages also vary:
some tools emit one aggregated `invalid_argument`, others emit field-specific
ones. **Fix:** a tiny `tool::ok(std::format(...))` helper + a per-tool
`OutputBuilder` that standardizes the shape.

**GAP-4.1.10 — `file.read` is missing line numbers + checksum.** The design doc
(`tool-runtime.md`) specifies `file.read` should return "content + line numbers
+ checksum". Current implementation returns content verbatim. Line numbers
matter for the agent because subsequent `file.edit` calls reference them; the
checksum matters for the read-then-edit TOCTOU defense.

**GAP-4.1.11 — `file.search` has no context lines.** No `-B`/`-A` equivalent.
For code search, the agent benefits enormously from seeing 2 lines before/after
each match — otherwise it re-reads the file to understand context.

**GAP-4.1.12 — No schema validation at tool registration.** `registry.add`
takes a `core::ToolDef` containing an `input_schema_json` string but never
verifies it parses as valid JSON Schema. A typoed schema fails silently at
registration and surfaces as confusing input rejection later.

### 4.2 Registry — hot path

**PERF-4.2.1 — `std::string` allocation per dispatch.** `Registry::find`,
`remove`, and `dispatch` all build a fresh `std::string` from the `string_view`
key to query the `unordered_map` (`registry.cpp:151, 160, 184`). Transparent
hashing (`unordered_map<string, Entry, string_hash, equal_to<>>`) makes this a
zero-allocation lookup. For every tool call.

**SMELL-4.2.2 — `Handler` is `std::function`.** Every dispatch pays an
indirection + a small-buffer-or-heap allocation cost on construction. For a
fixed-set built-in catalog, a `core::move_only_function` or a tagged variant
+ visit would be faster and cheaper to allocate. Probably not worth changing
for the open registry pattern (third-party tool libraries) but worth knowing.

**PERF-4.2.3 — Hook payloads do many string copies.** `build_after_payload`,
`build_before_payload`, etc. each copy `name`, `input_json`, `scope_key`,
`agent_key`, `identity` into the payload. The bus then `co_await`s sinks
sequentially; the payload could be `shared_ptr<const T>` so sinks share the
same allocation. For an N-sink subscription, the current cost is N copies; with
shared_ptr it's 1.

**STYLE-4.2.4 — `catalog()` copies all defs every call.** For a 50-tool
registry queried every agent turn (for the system-prompt tool-catalog render),
that's a full copy + allocation. A `std::ranges::view` returning const refs
to defs would suffice for the prompt builder.

**STYLE-4.2.5 — `Registry::find` comment is slightly inaccurate.** "valid
until the next `add` / `remove`" — actually, `unordered_map` add does *not*
invalidate pointers/references to values, only iterators (rehash). Only
`erase` invalidates pointers. Minor.

**SMELL-4.2.6 — `core::Time now{}` in `DispatchContext` defaults to the UNIX
epoch.** Intentional (so an uninitialized value cannot accidentally satisfy a
real TTL), but it's a footgun for tests that forget to set it — the test would
silently exercise the "always expired" branch. A factory method
`DispatchContext::for_now()` that calls `core::time::now_utc()` would prevent
the footgun.

### 4.3 Permission/Audit/Hook integration — well done

The dispatch pipeline in `registry.cpp:182-301` walks an audit/permission/hook
ordering that *correctly* handles every exit path:

| Exit path | `tool_before` | `tool_dispatched` | `tool_error` | `tool_after` |
|---|---|---|---|---|
| Unknown tool name | — | — | — | — |
| Audit record fails | ✓ | — | ✓ | ✓ |
| Verdict `deny` | ✓ | — | ✓ | ✓ |
| `ask` without broker (approval_required) | ✓ | — | ✓ | ✓ |
| `ask` + broker rejected | ✓ | — | ✓ | ✓ |
| `ask` + broker accepted + handler success | ✓ | ✓ | — | ✓ |
| `ask` + broker accepted + handler error | ✓ | ✓ | ✓ | ✓ |
| `allow` + handler success | ✓ | ✓ | — | ✓ |
| `allow` + handler error | ✓ | ✓ | ✓ | ✓ |

Audit happens **before** the handler so a forensic row exists even if the
handler crashes. The broker's rejection reason replaces the rule reason in the
audit row, preserving forensic precision. The `approval_required` error
carries `decision_reason / replay_max / approval_ttl_seconds` so the agent
loop can call `ApprovalBroker::approve` without re-running rule evaluation.

**This is the part of the codebase I'm most impressed by.** It is unusual to
see audit + permission + hook integration this well-ordered without
significant operational pain teaching the team where the off-by-one mistakes
live. Either the design got it right on paper, or there are real-world lessons
captured in the legacy audit.

### 4.4 IO layer

**SMELL-4.4.1 — `read_text_file_blocking` has a TOCTOU between
`ensure_readable_regular_file` and `std::ifstream`.** The stat could say
"regular file" but the file is replaced before the open. Low impact.

**SMELL-4.4.2 — Error mapping conflates types.** `system_io_error` maps
`std::errc::file_exists` to `conflict`, `permission_denied` to its own kind,
`no_such_file_or_directory` and `not_a_directory` both to `not_found`. The
`not_a_directory` case is questionable — it's an `invalid_argument` shape
(the caller gave us a path that isn't what they thought), not a `not_found`.

**STYLE-4.4.3 — `run_blocking` is a template defined in an anonymous
namespace.** Re-using it across handlers (per SMELL-4.1.4) would require
either exporting it or duplicating it. Exporting is the right call.

**GAP-4.4.4 — No file-watching, no glob, no subprocess.** The design doc
(`io-runtime.md`) reserves `oran-io` for "file/directory IO MVP — planned
glob, pipe, subprocess, signal". For the agent to call `shell.exec` (specified
in slice 0001's MVP scope), subprocess support has to land. This is a
significant amount of work (PTY, signal forwarding, cancellation,
streaming-output backpressure).

### 4.5 Async runtime

**SMELL-4.5.1 — Unusual `thread_pool`-hosts-`io_context.run()` pattern.**
`Runtime::Impl::run()` (`runtime.cpp:45-58`) posts `io_context.run()` to a
`thread_pool` of size `io_workers`, then joins. This works but is slightly
unconventional — the typical asio pattern is either a `thread_pool` with
direct posts, or N raw threads each calling `io_context.run()`. The current
pattern means each "task" on the pool is the blocking `io_context.run()`
call; the pool's executor is effectively unusable. **Why this matters:** if
anyone later does `asio::post(runtime.io_workers, ...)` thinking they're
posting work to the pool, they're enqueuing a task that runs after all
existing `io_context.run()` calls finish — i.e., never. Worth a clarifying
comment, or refactor to the more idiomatic pattern.

**SMELL-4.5.2 — `Runtime::stop()` flow.** Calls `work_guard.reset()` then
`io_context.stop()` then `cpu_workers.stop()`. The order is fine, but
`io_context.stop()` aborts in-flight handlers — which is what we want for
SIGINT, but a "graceful drain" `run_for(duration)` is not provided. For tests
that want to flush all pending work before shutdown, that's missing.

**GAP-4.5.3 — Mock clock for time-dependent tests.** Documented as deferred:
"A mock clock can land when the first scheduler/automation feature needs
deterministic virtual time." Will be needed when `oran-automation` lands.

### 4.6 Prompt / agent loop — biggest unknown

**The most architecturally important piece doesn't exist yet.** The design
docs are precise about what it must do:

- Render the prompt **once per turn** (memory section never recomputed
  per-iteration — legacy bug)
- Build `CacheSection` list with cache-version'd stable prefix, byte-identical
  across iterations
- Render tool catalog from `core::ToolDef`s (pure function — no clocks, no
  per-call IDs)
- Render deferred tools as name+one-liner (full schema lookup via `tool-search`)
- Render skills catalog separately (activating a skill must not break the
  preamble cache)
- Limit MAX_ITERATIONS to bound runaway tool loops

**Risk:** the cache-discipline rule (`rules/prompt-design.md`) is currently
unenforced. The `scripts/check-prompt-preamble` static grep is documented as a
tech-debt item ("waits on first stable preamble template"). The
`bench/oran-agent/prompt_cache_hit_rate.cpp` regression scenario is also
deferred. When the first preamble template lands, *the gate has to land
together* or the project will burn cache hits for many slices before noticing.

### 4.7 Storage — Pool concurrency

**SMELL-4.7.1 — Single mutex on the Pool.** `Pool::State::mutex` protects
writer/reader busy state + waiter queues (`pool.cpp:56-67`). Every
acquire/release walks the mutex. For low-concurrency workloads (one writer +
4 readers) this is fine. At scale (web mode with many concurrent agent
sessions), this becomes the contention point. **Defer until measured;**
flagged because v2's Web stretch goal is "asio-based, higher concurrency"
which would expose this.

**SMELL-4.7.2 — Waiter queues are FIFO, not priority-aware.** If a high-prio
agent (e.g., human REPL) is queued behind 100 low-prio automation jobs, it
waits 100 release cycles for a connection. Not actionable today; worth a
priority `enum` field once orchestration lands.

### 4.8 Hook bus — small issues

**SMELL-4.8.1 — `Bus::bind` allows duplicate sinks via different
`initializer_list` calls.** The check `std::ranges::contains(subscribers, &sink)`
guards against duplicates per-event, but each `bind` only checks the events in
*that* call. Correct on closer reading — the per-event check is sufficient.
Not a bug, scratch this.

**SMELL-4.8.2 — `Bus::publish_advisory` iterates sequentially even for
fully-independent advisory sinks.** N sinks → N sequential `co_await`s. For
hot events (`tool_after` on every tool call) with several sinks (an audit
sink + a metrics sink + a Slack sink), the total latency is the sum of all
sink latencies. **Fix:** for `publish_advisory`, use `asio::experimental::
make_parallel_group` or similar to fan out. Blocking publish (when it
arrives) must remain serial because the result is a vote.

**SMELL-4.8.3 — `binding_count()` and `sink_count(Event)` walk every map
entry every call.** Cache the totals if these become hot.

### 4.9 Documentation health

The `docs/` tree is **dense, current, and self-cross-referencing**. The
`STATUS.md` freshness gate (`check-status-fresh.sh`) and the histories
folder are excellent practices that should keep the docs from rotting.
Two minor concerns:

- The `ARCHITECTURE.md` "Slice status" header has grown to ~150 lines and is
  starting to drift from "scannable map" toward "release notes". The slice
  details belong in history files; the top-level table should just list
  current shape.
- Many design docs have status paragraphs interleaved with timeless content
  (e.g. `tool-runtime.md` lines 54-148 are slice-status, then design resumes).
  Future readers have to skim past the historical updates to find the canonical
  design. A `## Status` section at the top + a clean design body would scan
  better.

---

## 5. Forward-Looking — Better Agent, Better Tools

### 5.1 What a great agent loop should look like (forward design)

The MVP spec (`0001-core-react-loop.md`) is correct but **conservative**. Here's
the shape I'd advocate for, building on what's already there:

**Loop skeleton (~300 LoC target, not 4000):**
```cpp
Awaitable<Result<RunResult>> Loop::run(std::string user_prompt) {
  auto identity = identity_;                        // captured at construct
  auto cancel   = co_await this_coro::cancellation_state;

  // 1. Pre-render memory + skills sections (ONCE per turn, never per-iter).
  auto memory_section = co_await prompt_.render_memory(identity);
  auto preamble       = prompt_.preamble(identity, memory_section);

  for (std::size_t iter = 0; iter < cfg_.max_iterations; ++iter) {
    if (cancel.cancelled()) co_return Error::cancelled();

    co_await hooks_.publish_advisory(Event::iteration_start, {iter, identity});

    // 2. Build the request from preamble + tool catalog + skills + tail
    auto request = prompt_.build(preamble, working_.scratchpad, user_prompt);

    // 3. Provider call — single round trip, streaming OK
    auto response = co_await provider_.send(request, identity);
    if (!response) co_return std::unexpected(response.error());

    // 4. For each tool_use in the response, dispatch
    for (const auto& call : response->tool_calls()) {
      DispatchContext ctx{...};
      auto out = co_await tools_.dispatch(call.name, call.input, ctx);
      working_.scratchpad.append(out);             // success OR error
    }

    if (response->stop_reason == StopReason::end_turn) {
      co_await hooks_.publish_advisory(Event::final_response, {response, identity});
      co_return RunResult{*response, working_};
    }
  }

  co_return std::unexpected(Error::iteration_limit_exceeded());
}
```

The interesting questions are *not* in the loop body — they're in:

1. **Streaming.** Tool calls should be dispatchable *as the LLM emits them*
   (not after `stop_reason`). The provider adapter's incremental parse needs
   to yield tool-use events.
2. **Parallel tool dispatch.** When the model emits multiple `tool_use` in one
   turn (allowed by Anthropic Messages), they should run in parallel via
   `asio::experimental::make_parallel_group`. Serial dispatch is the legacy
   default and an obvious throughput loss.
3. **Per-tool timeout budget.** Each `dispatch` should have a per-call wall-clock
   budget surfaced as a cancellation signal. Tools that blow the budget get
   `Error::tool_timeout`; the agent observes it as a normal failure.
4. **Total-spend budget.** Aggregate `cost_estimate` across calls; emit a hook
   event when crossing thresholds (the design doc's "Provider Cost Awareness"
   sketch).
5. **Conversation tail compaction.** When the tail grows past N tokens, run a
   summarization on the older half via `cpu_executor`. This is the missing
   piece between session memory and working memory.

### 5.2 Tool design — design rules for new tools

The current six tools are well-shaped *for what they do*, but each was hand-
written with its own JSON parsing, validation, and output format. Future tool
authors will copy-paste those patterns and the drift will continue. **Pre-empt:**

**Rule 1 — Tools declare a typed input struct, not a JSON string schema.**
```cpp
struct FileEditInput {
  std::filesystem::path path;
  std::string           old_string;
  std::string           new_string;
  bool                  replace_all = false;
  static constexpr auto schema_id = "file.edit/v1";
};

// In the tool registrar — schema generated from the struct via C++26
// reflection (GCC 16.1 supports it).
tool::register_typed<FileEditInput>(registry, "file.edit", description,
                                    {Capability::edit_file},
                                    &handle_file_edit);
```
The framework owns: JSON parsing, field validation, schema generation, error
message shape. The tool body sees a populated `FileEditInput&`. This kills
~120 LoC of boilerplate today and prevents drift on every future tool.

**Rule 2 — Tools have a `SafetyEnvelope` that wraps timeout, cancel-poll,
size caps, encoding checks.** Caller writes:
```cpp
register_typed<...>(registry, ..., safety::file_handler(
    .max_input_bytes  = 256,
    .max_output_bytes = 16 << 20,
    .deadline         = 5s,
    .cancel_poll      = 10ms
));
```
Means tools never have to remember to check cancellation themselves.

**Rule 3 — Tool output is structured, not strings.**
```cpp
struct Output {
  std::string           text;          // primary human-readable
  std::optional<nlohmann::json> data;  // structured (for chained calls)
  std::vector<Attachment> attachments; // files / images
  std::optional<TokenCost> cost;       // for provider-cost accounting
  bool is_error = false;
};
```
Current `Output { text, is_error }` is the v1 minimum. Don't ship more tools
without bumping this — every future tool that needs structured output gets
duplicated string-parsing on the consumer side.

**Rule 4 — One unified `file.modify` instead of per-kind tools.** The
project's own `STATUS.md` and `file-delete` history already commit to this
direction. Concretely:
```jsonc
// file.modify
{
  "action": "delete" | "rename" | "chmod" | "mkdir" | "touch",
  "path":   "...",
  "to":     "...",          // for rename
  "mode":   "0644",         // for chmod
  "recurse": true | false,  // for delete/mkdir
  "follow_symlinks": true | false
}
```
Plus a unified `directory.scan` (recursive `directory.list`) that returns a
single tree response. This replaces the current six tools with two over the
next refactor — fewer entries in the system prompt, fewer specialised
capabilities, fewer rules to write.

**Rule 5 — A dedicated `code.*` tool family for code-aware ops.** The agent
will spend most of its time editing code. Specialised tools beat generic
file tools here:

- `code.symbols(path)` — return the symbol map (functions, types, modules)
  for a file. Faster than re-reading, and the LLM can navigate without
  loading the whole file.
- `code.references(symbol, scope)` — find all references. Replaces
  `file.search` for code with a much smaller, focused result.
- `code.diff(path, before, after)` — render a unified diff. The LLM uses it
  to verify its own edits.
- `code.format(path, formatter)` — gated through `permission`. Saves the
  agent from re-running clang-format manually.

These ride on top of LSP / tree-sitter; a slice for `oran-lsp` integration
would precede them.

**Rule 6 — Streaming output for long-running tools.** Right now,
`Handler` returns `Awaitable<Result<Output>>` — one shot, no progress. A
streaming variant for long-running tools (a multi-second `code.format` on a
big tree, a long `shell.exec`) lets the agent emit partial progress updates
to the user without losing the loop. `Awaitable<async::Generator<Output>>` is
the shape.

### 5.3 Hooks — the right extension surface

The hook bus is poised to be the project's killer feature *if* the blocking
veto path lands cleanly. Today everything is advisory — sinks observe but
cannot change behavior. The blocking variant, when it arrives, unlocks:

- **Custom permission UIs.** A `permission_ask_rendered` hook sink can pop a
  dialog in a desktop integration, return the user's choice through
  `EventTraits<Event::permission_ask_rendered>::Decision`.
- **Pre-flight validation.** A `tool_before` veto sink can run additional
  domain-specific checks (e.g., "don't let the agent touch *.production.env"),
  outside the rule engine.
- **Input rewriting.** A `tool_before` rewrite sink can normalize paths,
  resolve `~`, lowercase, etc. without each tool reimplementing it.
- **Cross-cutting policy.** A `memory_write_before` veto sink can prevent
  PII from being persisted to long-term memory.

**Priority recommendation:** ship `publish_blocking` + `Decision` types as
soon as `oran-agent` lands. The 80-LoC implementation cost cited in tech-debt
is right; the *unlock value* is much larger than that.

### 5.4 Skills — start small, ship soon

The `oran-skill` library is a `0009-skills.md` deliverable but hasn't started.
A useful MVP that fits inside one slice:

- Read `<workspace>/.orangutan/skills/*.md` at agent startup
- Parse YAML frontmatter (`name`, `description`, `triggers`) + body
- Render skill catalog as section (4) of the prompt (name + description only;
  body is *not* in the preamble — that's the cache-discipline rule)
- A `skill.activate` tool (one of the deferred ones) that pulls in the body
  for the current turn

The hot-reload (inotify watcher) is a stretch; the catalog itself is the
unlock. This is **the highest-value missing tool capability today** —
without skills, the agent has no way to import operator-defined playbooks.

### 5.5 Memory — design defers vector for good reason

The design doc is right to *not* build a vector DB from scratch. Concrete
plan I'd advocate:

1. Ship FTS5 backend first (single-slice deliverable, the heavy code is in
   `storage::Pool` already).
2. Decay policy as a periodic job (waits on `oran-automation`).
3. MEMORY.md mirror as an opt-in config flag.
4. Vector backend as a stretch goal, plugged in through `Backend` trait —
   `sqlite-vec` is the obvious local option, an HTTP embedding-service
   adapter for remote.

The legacy lesson — **memory section computed once per turn, never per
iteration** — is already baked into `agent-platform.md` and `memory-system.md`.
Just need the agent loop to enforce it.

### 5.6 Provider portability — the hidden complexity

`oran-provider` doesn't exist yet but `api-portability.md` exists. The
hidden complexity: **prompt cache mapping**. Different providers cache
differently:

- Anthropic: explicit `cache_control` blocks (max 4 breakpoints, ≥1024 tokens
  per block)
- OpenAI Responses: implicit prefix hashing (no markers; provider auto-detects
  identical prefix)
- Gemini: explicit cached content API (separate from request)

The `CacheSection` abstraction in `api-portability.md` handles this, but the
**adapter MUST validate** that the section layout the prompt builder
produced actually maps to the provider's constraints. A 200-token section on
Anthropic Sonnet wastes a `cache_control` entry; on OpenAI it just doesn't
cache. The provider adapter should reject section layouts that violate the
provider's cache rules at construction time, not at the first request.

---

## 6. Prioritized Recommendations (action items)

### P0 — should land within the next 1–2 slices

| # | Action | Why | Effort |
|---|---|---|---|
| 1 | **Add atomic-write to `file.edit`** (write-to-temp + rename) | Data-loss footgun; LLMs use this constantly | S |
| 2 | **Add content-size caps to `file.write` and `file.edit`** | OOM safety; mirror `ReadTextOptions::max_bytes` | XS |
| 3 | **Transparent hashing on `Registry::entries_`** | Removes a `std::string` allocation per dispatch | XS |
| 4 | **Validate JSON schema at `Registry::add`** | Catches typos at registration, not at first call | XS |
| 5 | **Cancellation polling in `file.search` walk** | Pathological walks become uncancellable today | XS |

### P1 — should land before the next big subsystem (oran-agent)

| # | Action | Why | Effort |
|---|---|---|---|
| 6 | **Land `publish_blocking` + `EventTraits<E>::Decision`** | Unlocks operator-prompt sink + tool-before veto | S |
| 7 | **Extract `tool::parse_input<T>` helper** | Removes ~120 LoC + a class of future drift | S |
| 8 | **Ship `code.symbols` / `code.references` tools** (via LSP) | Highest-ROI tool family for code-editing agents | M |
| 9 | **Ship MVP `oran-skill` (markdown loader + catalog renderer)** | Unblocks operator playbooks before agent loop lands | S-M |
| 10 | **Bench the prompt-cache hit rate** with first preamble template | The discipline rule is unenforced today | S |

### P2 — design-influencing decisions

| # | Action | Why | Effort |
|---|---|---|---|
| 11 | **Refactor toward unified `file.modify` + `directory.scan`** | Already-committed direction; do it before more file tools land | M |
| 12 | **Bump `Output` to {text, data, attachments, cost, is_error}** | Don't ship more tools until shape is final | S |
| 13 | **Move blocking dispatch to parallel sink fan-out** | Latency win on hot hook events | S |
| 14 | **`io::run_blocking` exposed as public utility** | Removes the only `<fstream>` use in `oran-tool` | XS |
| 15 | **`code::Time` factory `for_now()` in `DispatchContext`** | Avoids the UNIX-epoch-default test footgun | XS |

### P3 — measure first, fix later

| # | Action | Why | Effort |
|---|---|---|---|
| 16 | **Bench `Pool` mutex contention under concurrency** | Single-mutex design works today; web mode may expose it | M |
| 17 | **`Runtime::Impl::run()` clarification or refactor** | Currently unusual pattern; document or fix | XS |
| 18 | **`shared_ptr<const Payload>` for hook publish** | Performance optimization for hot multi-sink events | S |
| 19 | **`scripts/check-prompt-preamble`** | Mechanical enforcement of cache discipline | S |
| 20 | **Vector backend trait + `sqlite-vec` adapter** | Long-term memory search quality | M-L |

---

## 7. The Vision — Where This Project Should Aim

The current architecture is well-suited for **a great single-developer coding
assistant**. The interesting question is: *can it evolve into the platform
the design doc says it wants to be?* The platform vision is more ambitious:

1. **Programmable coordination strategies** — leader-worker / pipeline /
   voting / free-form as pluggable `Strategy` classes
2. **Conversation DAG as first-class** — multi-agent runs accumulate a graph
   stored in `orchestration.db`; post-hoc replay/audit/learning
3. **Skill hot-reload** — inotify watcher on `<workspace>/.orangutan/skills/`
4. **Provider cost awareness** — per-`agent_key` spend tracking + hook
   thresholds + budget-exhausted fallback
5. **Tool capability discovery** at registration — already implemented (good!)
6. **Channel bidirectional threading** — Discord/Slack threads as
   sub-sessions in the agent's session store
7. **Self-reflective reports** — `task-debrief.md` emitted after long tasks,
   readable by the next task as working memory

These are **all expressions of the same idea:** the runtime is a place where
agents accumulate knowledge over time and operators extend behavior without
forking. The hook bus + permission engine + skills + memory tiers are the
four legs of that table.

What concerns me less than the technical risk: **product clarity**. The doc
is very clear about anti-goals ("not a SaaS, not a high-QPS proxy, not a
substitute for Claude Code"). The clarity will help every future slice say
no to feature creep.

What concerns me most: **the gap between the high-quality foundation and the
unbuilt agent surface**. Foundation libraries are easy to keep clean because
they have no UX. The agent loop, the prompt builder, the provider adapter —
these are where compile-budget violations, copy-paste, and abstraction
sprawl tend to land. The discipline that produced today's `oran-permission`
needs to hold through `oran-agent` and `oran-provider` slices. If those land
clean, v2 will be a genuinely different project from its legacy ancestor.

---

## 8. Closing Notes

- Total LoC examined: ~11.6 kLoC source + ~1500 LoC public headers + ~12k
  docs.
- Tests examined indirectly through the assertion counts in `STATUS.md`
  (~2855 assertions across all libs). Direct test-file review was not done
  for this report; quality of test coverage is therefore not assessed —
  flagged for follow-up.
- I did *not* attempt to compile the project; all findings are static.
  Several PERF claims would benefit from being benched before acting on.
- No code changes were made.

---

## 9. Critical Addendum — Lower-Layer Defects (post-synthesis discovery)

After the main report was drafted, a second parallel exploration surfaced
**more severe defects** in `oran-async`, `oran-storage`, and `oran-hook` than
§4 records. These bump the P0 list and warrant their own section because they
affect correctness, not ergonomics. I verified each by reading the cited code
before listing it.

### 9.1 `oran-async::Channel<T>` — multiple real bugs

**Verified by reading `include/oran/async/channel.hpp`.**

**BUG-9.1.1 — Cancellation slot race.** `async_send` and `async_receive`
both assign the cancellation slot (lines 108–119, 138–149) *before*
taking `mutex_` to push onto the waiter queue (lines 121–129). If a
cancellation signal fires in that gap, `cancel_send(id)` runs against an
empty `senders_` (no entry with that id yet), silently drops the cancellation,
and *then* the push completes. The result is a stuck waiter with a
slot that has already fired and will not fire again. **The same class
of bug is explicitly defended against in `Pool::async_acquire_writer`
(`pool.cpp:178-188`) — `Channel` did not get the same fix.**

**BUG-9.1.2 — Handler's associated executor is ignored.**
`make_send_complete` / `make_receive_complete` (`channel.hpp:202–220`)
capture `executor_` (the *channel*'s executor) and `asio::post` completions
there:
```cpp
auto completion_executor = executor_;
return [completion_executor, handler = std::forward<Handler>(handler)](core::Result<void> result) mutable {
  asio::post(completion_executor, [handler = std::move(handler), result = std::move(result)]() mutable {
    std::move(handler)(std::move(result));
  });
};
```
This breaks asio's contract that handlers run on
`asio::get_associated_executor(handler, default)`. A handler bound to a
**strand** will be resumed off-strand and observe data races against any
other coroutine sharing the strand. **High-impact:** strands are the
project's primary serialization mechanism (one strand per agent loop,
one strand per session DB writer); any code path that does
`co_await channel.receive()` from a strand-bound coroutine has a latent
race.

**Fix sketch:** capture `asio::get_associated_executor(handler, executor_)`
(falling back to `executor_` when the handler has none) instead of the
channel's executor. Mirror the fix in both `make_send_complete` and
`make_receive_complete`.

**PERF-9.1.3 — Completions are wrapped twice.** Every wakeup builds a
`Deferred` that *synchronously* invokes the inner callable, which then
does `asio::post`. So `pump_locked` adds one indirection per completion
plus one `asio::post` per completion — high per-op cost on a hot
channel. Fixing 9.1.2 (use the handler's associated executor +
`asio::dispatch` when possible) tends to clean this up too.

### 9.2 `oran-hook::Bus::publish_advisory` — exception leak

**Verified by reading `src/oran-hook/bus.cpp`.**

**BUG-9.2.1 — Sink exceptions escape the publish loop.** `bus.cpp:53–69`:
```cpp
for (auto* sink : it->second) {
  auto result = co_await sink->receive(event, payload);
  ...
}
```
There is no try/catch. `InProcessSink::receive` forwards a user-supplied
`std::function<async::Awaitable<core::Result<void>>(...)>` — if the user's
callback throws (or its awaitable propagates an exception), the publish
**aborts mid-iteration** and the exception propagates out of
`publish_advisory`. The header at `bus.hpp` documents the advisory contract
as "do not abort the publish for subsequent sinks" — the code does not
honor it.

**Why it matters:** the hook bus is meant to be a *defense* against
misbehaving extensions. The contract is "advisory sinks observe but cannot
change behavior." Today, a single buggy sink kills the audit hook for every
subsequent sink, and the exception travels up into tool dispatch's
`[[maybe_unused]] auto after_outcome = co_await ...publish_advisory(...)`,
which has no exception handler either. **Tool dispatch can crash if any
advisory sink throws.**

**Fix:** wrap the `co_await sink->receive(...)` in try/catch (or use
`co_await`-aware exception capture) and stamp the caught exception into
`PublishOutcome::SinkResult::error` as a synthetic `Error::internal`.

**SMELL-9.2.2 — Payload copy on publish.** `payload` is captured by value
in the awaitable frame and passed by reference to each sink — actually OK,
no per-sink copy. (Subagent originally flagged this; I disagree after
re-reading.) However, the `payload` itself is copied once at publish
(passed by value, not move). Move on the last iteration would save one copy.

### 9.3 `oran-storage::StatementCache` — orphan path is silent, not leaky

**Verified by reading `src/oran-storage/statement_cache.cpp`.**

**SMELL-9.3.1 — Orphan statements are silently dropped, no metric bump.**
When all cached entries are leased and `acquire` is called for a new SQL
(`statement_cache.cpp:166–170`), the new statement is created, marked
`orphaned=true`, and returned. On `CachedStatement::release` (line 237),
orphaned entries return early — the underlying `Statement` is destroyed (which
calls `sqlite3_finalize`, so **no leak**), but `evictions` is not
incremented and there's no log. Under any sustained contention, callers
get correct results but the cache hit-rate metrics lie. **Severity: SMELL,
not BUG** (subagent flagged as BUG; verified there is no leak — the
`Statement` RAII handles cleanup).

**Fix:** add a `state.orphan_misses++` counter or a `oran-log` warning when
the orphan branch fires.

### 9.4 `oran-async::Runtime` — confusion, not bugs

**Verified by reading `src/oran-async/runtime.cpp`.**

The subagent flagged `Runtime::run()` as blocking forever and
`cpu_workers` as ignoring its config. **Both are wrong on careful read:**

- `run()` blocking until `stop()` is the **intended contract** — the
  function is "host the runtime until shutdown". The `work_guard` keeps
  `io_context` alive; `stop()` releases it and the join unblocks.
- `cpu_workers(config.cpu_workers)` (line 31) IS passing the configured
  size as the `asio::thread_pool` constructor's `num_threads` argument —
  same as `io_workers`. The pool is started immediately on construction.
- Callers post to it via `cpu_executor()` (`async::post(rt.cpu_executor(), ...)`).
  Nothing in `run()` needs to post to it.

**However, the comments + naming are confusing enough that a future reader
will repeat this confusion.** The `io_workers` thread_pool exists *only* to
host `io_context.run()` calls — that's an unusual pattern and worth a
docstring explaining "we use the thread_pool's join() as our shutdown
synchronization point; nobody else posts to io_workers."

### 9.5 Doc drift — `38 events` should be `41 events`

**Verified by reading `include/oran/hook/event.hpp` and counting.**

Multiple docs (`STATUS.md`, `permissions-and-hooks.md`,
`tool-runtime.md`) refer to "38 lifecycle events." The actual enum in
`event.hpp:25–76` is **41 enumerators**: 5 agent + 4 provider + 4 tool +
6 memory + 6 channel + 7 orchestration + 4 automation + 2 session + 3
permission = 41. Trivial doc fix but symptomatic of where the Prime
Directive can quietly drift when an enum grows by one between commits.

### 9.6 Other storage/hook items worth checking (unverified by me)

The subagent flagged additional items I did not re-read source for. Listed
as **flagged for the human reviewer** rather than asserted:

- `PRAGMA synchronous=NORMAL` requirement from `secrets-and-state.md` is
  not being set on reader connections (per subagent reading of
  `sqlite.cpp:291–333`). Worth checking before the production storage path
  goes live.
- No `PRAGMA cache_size / mmap_size / wal_autocheckpoint /
  journal_size_limit` tuning anywhere. Per-workload tuning is a v2
  question, but defaults should at least be documented.
- `AuditRepository::list_events` constructs SQL via concatenation
  depending on filter shape — yields up to 16 distinct statement-cache
  entries instead of one parameterized statement. Cache pressure on hot
  list calls.
- Repositories store `Pool*` raw — if `Pool` moves, pointer dangles. The
  pool was move-defaulted (`pool.hpp:91` per subagent). **Verify and
  delete the move constructor if the pointer-store pattern is by
  design**, else introduce a `shared_ptr<Pool>` or stable `Pool::handle()`.
- `migrations.cpp` does not record a SQL checksum/hash in
  `schema_versions`. Add one to detect SQL drift between releases.

### 9.7 Revised P0 priorities

The §6 P0 list should be reordered as:

| New rank | Action | Severity | File:line |
|---|---|---|---|
| **0** | **Fix `Channel` handler-executor bug** | BUG, data race | `channel.hpp:202–220` |
| **0** | **Fix `Channel` cancellation slot race** | BUG, stuck waiter | `channel.hpp:108–129, 138–161` |
| **0** | **Wrap `publish_advisory` sink calls in try/catch** | BUG, dispatch crash | `bus.cpp:53–69` |
| **0** | **Fix doc drift `38 events → 41 events`** | DOC, Prime Directive | multiple |
| 1 | Atomic-write to `file.edit` | data-loss | `file_edit.cpp` |
| 2 | Content-size caps on `file.write`/`file.edit` | OOM | trivial |
| 3 | Transparent hashing on `Registry::entries_` | hot-path perf | trivial |
| 4 | Validate JSON schema at `Registry::add` | dev UX | trivial |
| 5 | Cancellation polling in `file.search` walk | UX | trivial |

**These four "rank 0" items are the most important findings in this entire
report.** The async/hook bugs would silently corrupt the agent loop the
moment it ships, in ways that would be very hard to diagnose post-deploy
because they're race-condition shaped.

---

## Appendix A — Files Read (for reproducibility)

Foundational docs:
`STATUS.md`, `ARCHITECTURE.md`, `PRODUCT_SENSE.md`,
`design-docs/core-beliefs.md`, `rules/critical-rules.md`,
`design-docs/agent-platform.md`, `design-docs/tool-runtime.md`,
`design-docs/async-model.md`, `design-docs/permissions-and-hooks.md`,
`design-docs/memory-system.md`, `references/orangutan-legacy-audit.md`,
`rules/prompt-design.md`, `product-specs/0001-core-react-loop.md`,
`exec-plans/tech-debt-tracker.md`,
`histories/2026-05/20260520-2345-oran-tool-file-delete.md`.

Source:
`src/oran-tool/{registry,builtins,file_read,file_write,file_edit,file_search,
directory_list,file_delete}.cpp`,
`include/oran/tool/registry.hpp`,
`src/oran-io/file.cpp`, `include/oran/io/file.hpp`,
`src/oran-async/{runtime,sleep}.cpp`,
`src/oran-hook/bus.cpp`,
`src/oran-storage/pool.cpp` (first 100 lines).

Plus parallel subagent exploration of `oran-tool` dispatch and built-ins
(detailed report incorporated above).

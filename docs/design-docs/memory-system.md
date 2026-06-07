# Memory System

Orangutan v2 makes memory **tiered and explicit**. Each tier has its own backend trait,
its own retention policy, and its own hookable lifecycle. The legacy `orangutan/`
"memory" was a single SQLite store + a MEMORY.md mirror with no concept of tiers; v2
turns that into four distinct concerns.

## Tiers

| Tier        | Lifetime           | Scope                              | Backing                  |
| ----------- | ------------------ | ---------------------------------- | ------------------------ |
| **Working** | one ReAct turn     | this agent, this prompt            | in-process               |
| **Session** | one session        | this agent + this `session_id`     | `sessions.db` (SQLite)   |
| **Long-term** | persistent       | this agent (scope_key)             | `memory.db` (SQLite + FTS5 + optional vector) |
| **Shared**  | persistent         | this team (`team_id`)              | `memory.db`, separate table |

Hooks fire on writes/reads at each tier (see "Hook Surface" below).

## Working Memory

Built up while a single ReAct turn executes. It is the place where:

- The pre-rendered memory section lives (so the prompt is built once per turn).
- Tool outputs accumulate before they are summarized.
- The current iteration's plan / scratchpad / partial answer sits.

Backed by in-process state, owned by `agent::Loop`. Cleared at turn end.
No SQLite involvement.

The section-5 prompt memory bytes have a small public owner in `oran-memory`:
`memory::FramingOwner` holds a `memory::Framing { section_text }` value and
renders it at the runner boundary once per prompt. Exact callers may still
supply already-materialized text; slice 164 adds an opt-in
`AgentPromptRunnerOptions::longterm_recall` path that populates the same value
from recalled long-term records before the loop starts, and slice 165 maps
configured-route `memory.longterm.recall` policy into that option. Slice 166
extends that policy with optional `kinds` filters over `RecordKind` spellings,
and slice 167 adds a `query_strategy` selector so configured runs can keep the
default current-prompt query or recall from the last user message for follow-up
prompts.
`AgentPromptRunner` copies that rendered string into `RunTurnInputs::memory_framing`, so
`agent::Loop` may rebuild `prompt::Builder` sections across provider/tool
iterations without re-querying memory.

Loop-local working state remains private to `agent::Loop`:

```cpp
class Loop {
  struct Working {
    std::string rendered_memory_section;       // computed before the loop
    std::vector<core::Content> scratchpad;     // mutated as the loop runs
    std::optional<std::string> plan;           // if the agent emitted one
  };
};
```

## Session Memory

Conversation history. The legacy `SessionStore` is preserved in spirit: per-session,
per-agent JSON-serialized message stream in SQLite. v2 changes:

- **Expected-only API.** `core::Result<...>` everywhere. No `must_ok` wrappers.
- **One DB connection per writer**, pool for readers; WAL on.
- **Prepared-statement cache** (legacy didn't have one).
- **Schema migrations** versioned and applied at startup.
- **Append-only fast path**: appending a message is one INSERT; loading is one SELECT.

Storage foundation status: `oran-storage::SessionRepository` implements the
`sessions.db` schema, append/load/get/list operations, and hot SQL through
`Pool` slot `StatementCache`s. Its schema loads from
`src/oran-storage/migrations/sessions/0001-sessions-initial.sql`. It stores
`content_json` and `metadata_json` as opaque strings but types `role` as
`core::Role` at the API boundary.

Memory wrapper status (slice 130+148): `oran-memory::session::Store` is now the
typed memory-layer owner over `SessionRepository`, and `RuntimeAssembly` now owns
the configured-route sessions DB pool/repository/store. The wrapper serializes
`core::Message` / `core::Content` privately in `src/oran-memory/session.cpp`,
using versioned JSON for text, thinking, tool-use, and tool-result blocks while
keeping `oran-storage` unaware of message shape. It validates non-empty
session/agent ids, maps repository summaries into typed session summaries, wraps
durable skill activation updates/records, and returns parsing errors for malformed
stored rows. `RuntimeAssembly` opens and migrates
`<workspace>/.orangutan/sessions.db` separately from `audit.db`, exposes
`session_store()`, and lets built-in no-route startup disable the store so fresh
deterministic CLI runs do not create session state. `AgentPromptRunner` now loads
persisted history and durable skill activation rows before each prompt, then appends
the successful transcript suffix and records successful `skill.invoke` /
`skill.deactivate` activation updates through that owner when session memory is
enabled; the in-process transcript remains the fallback when it is not.
`memory::FramingOwner` separately owns prompt section 5 and is rendered once by
`AgentPromptRunner` before each loop turn.

```cpp
// include/oran/memory/session.hpp
namespace orangutan::memory::session {

struct SessionId  { std::string value; };
struct AgentKey   { std::string value; };
struct ListSessionsOptions { AgentKey agent_key; std::size_t limit = 50; };
struct SessionSummary {
  SessionId session_id;
  AgentKey agent_key;
  std::size_t message_count;
  std::string created_at;
  std::string updated_at;
};

struct SkillActivationUpdate {
  std::string name;
  bool active;
};

struct SkillActivationRecord {
  std::string name;
  bool active;
  std::string created_at;
  std::string updated_at;
};

class Store {
 public:
  explicit Store(storage::SessionRepository&);

  async::Awaitable<core::Result<void>>
  append(SessionId, AgentKey, core::Message);

  async::Awaitable<core::Result<std::vector<core::Message>>>
  load(SessionId, AgentKey);

  async::Awaitable<core::Result<void>>
  record_skill_activation(SessionId, AgentKey, SkillActivationUpdate);

  async::Awaitable<core::Result<std::vector<SkillActivationRecord>>>
  load_skill_activations(SessionId, AgentKey);

  async::Awaitable<core::Result<std::vector<SessionSummary>>>
  list(ListSessionsOptions);
};

}  // namespace orangutan::memory::session
```

## Long-Term Memory

Per-agent persistent facts (about the user, the project, ongoing tasks, learned
patterns). The legacy implementation: SQLite + FTS5 + a single mutex + an optional
MEMORY.md mirror. v2 keeps that core and adds:

- **Connection pool** for reads; one writer connection on a strand.
- **Vector backend slot** (interface, optional): `longterm::Backend` owns the
  lexical/record store seam, and `longterm::VectorBackend` owns the embedding
  index seam that an optional sqlite-vec / HNSW / external adapter can implement.
- **Typed kinds** match the legacy ones (user, feedback, project, reference) and gain a
  fifth: `team` (mirrors of shared-tier records for cross-tier search convenience).
- **Decay policy**: `memory-age` style decay will consume explicit read-touch
  metadata before pruning. Expired records eventually receive lower search weight
  before potentially being shadowed or deleted.

Status (slice 189): `include/oran/memory/longterm.hpp` now ships the public
record/query/write/touch/decay shapes, reflection-backed `RecordKind`,
`Backend` and `VectorBackend` traits, validation helpers for record keys,
search limits, record metadata, touch requests, decay requests, and vector
embeddings, plus `Fts5Backend` as the default SQLite FTS5 lexical backend.
`Fts5Backend` owns the long-term memory schema in
`src/oran-memory/migrations/longterm/`, applies its built-in migration through
the shared `storage::Pool` writer, returns `ErrorKind::not_found` for missing
`get(...)` rows, advances `last_read_at` monotonically through `touch(...)`,
marks decay candidates as `shadow=true` through `decay(...)`, and treats
`remove(...)` as idempotent. Slice 187 adds the `oran-automation` planner that
can produce due-only `DecayRequest` values for future periodic retention
producers without moving scheduling ownership into `oran-memory`. Slice 188 has
bootstrap map configured retention policy into that automation-owned job
descriptor and expose it from `RuntimeAssembly`; slice 189 adds
automation-owned job/run/last-fired persistence in `oran-automation`. The memory
library still only owns the backend execution primitive.
Slice 162 adds
`longterm::Runtime`, a prompt-boundary composition layer that delegates search
to a `Backend`, validates recall requests before dispatch, and renders stable
`memory::Framing` bytes from returned hits with `render_recall_framing(...)`.
Slice 163 wires `RuntimeAssembly` to own a separate
`<workspace>/.orangutan/memory.db` pool, migrate `Fts5Backend`, expose
`longterm_memory_backend()` / `longterm_memory_runtime()`, and enable that state
only for configured-route bootstrap startup. Slice 164 adds an explicit
`AgentPromptRunnerOptions::longterm_recall` opt-in that queries that runtime once
at the prompt boundary and feeds the resulting deterministic framing into section
5 before `agent::Loop`. Slice 165 adds the first config policy:
`memory.longterm.recall.enabled` defaults false, `memory.longterm.recall.limit`
defaults 5, and ordinary configured-route `bootstrap::run` maps those fields
into `AgentPromptRunnerOptions::longterm_recall`. Slice 166 adds optional
`memory.longterm.recall.kinds`, a non-empty unique array of `RecordKind`
spellings (`user`, `feedback`, `project`, `reference`, `team`) that constrains
the prompt-boundary query; omitting it keeps the previous all-kind search. Slice
167 adds `memory.longterm.recall.query_strategy`, with `prompt_text` preserving
the current-prompt query and `last_user_message` deriving the single recall
query from the most recent previous user text when one exists. Slice 168 adds
`render_recall_data_json(...)` for structured recall record metadata and the
read-only `memory.recall` tool path, which reaches the same runtime through
bootstrap's `DispatchContext::memory_recall` callback and the ordinary
permission/audit/hook/output-cap pipeline. Slice 169 adds
`render_remember_data_json(...)` for structured saved-record metadata and the
write-side `memory.remember` tool path, which reaches the same assembly-owned
long-term `Backend` through `DispatchContext::memory_remember`; bootstrap
stamps the runner's stable scope key plus dispatch-time timestamps before
upserting. Slice 170 adds `render_forget_data_json(...)` for scoped removed-key
metadata and the delete-side `memory.forget` tool path, which reaches the same
assembly-owned long-term `Backend` through `DispatchContext::memory_forget`;
bootstrap derives the runner's stable scope key before calling the backend's
idempotent `remove(...)`. Slice 171 adds the first 10k-record FTS5 search bench:
`bench/memory/scenarios/longterm_fts5.cpp` seeds the default backend once and
measures `longterm::Runtime::search("react agent loop", limit=10)` as the
baseline future sqlite-vec and hybrid-ranking work must compare against. It does
**not** ship the gated sqlite-vec adapter. Slice 172 adds the first
`longterm::HybridRuntime` composition contract: callers provide one lexical
`Query`, one `VectorEmbedding`, per-backend limits, a result limit, and
non-negative lexical/vector weights; the runtime validates the request, searches
the lexical `Backend` and vector `VectorBackend`, hydrates vector-only keys
through `Backend::get`, ignores stale vector rows whose records are missing,
and returns deterministically sorted `SearchHit` rows with weighted combined
scores plus populated lexical/vector score components. `HybridRuntime::recall`
reuses the same stable recall framing renderer. Slice 174 adds the
operator-facing config contract for downstream hybrid wiring:
`memory.longterm.hybrid_search.enabled`, positive `lexical_limit`,
`vector_limit`, and `result_limit`, and non-negative finite `lexical_weight` /
`vector_weight` with at least one non-zero weight. Slice 175 made the no-vector
boundary explicit at configured-route startup. Slice 176 adds the library-level
sqlite-vec `VectorBackend` adapter behind `--vector_memory=y`, and slice 178
wires the gated bootstrap owner: when the binary is built with
`--vector_memory=y`, `RuntimeAssembly` opens a separate
`<workspace>/.orangutan/memory-vectors.db`, migrates `SqliteVecBackend`,
constructs `HybridRuntime`, and `AgentPromptRunner` uses a deterministic local
text embedding owner (`oran-local-text-v1`, 64 dimensions) for prompt-boundary
recall and `memory.recall`. The same runner mirrors `memory.remember` writes and
`memory.forget` deletes into the vector index. Slices 179 and 180 wire the first
long-term memory lifecycle hooks at the bootstrap callback boundary:
`memory.remember` publishes blocking `memory_write_before` after parsing and
scoping the record but before any lexical/vector backend mutation, then
publishes advisory `memory_write_after` after a successful write. A veto returns
`ErrorKind::permission_denied` with `reason=blocked_by_hook` and skips both
backend writes; rewrite and require-approval decisions are explicitly rejected
as unsupported for this consumer. `memory.forget` publishes advisory
`memory_forget` after a successful scoped delete. Prompt-boundary long-term
recall and the `memory.recall` tool publish advisory `memory_read_after` after
successful lexical or hybrid recall. Default hook sinks receive redacted
memory payloads: writes omit record title/body/tags/linked ids, reads omit raw
query text plus hit title/body/tags/linked ids while preserving byte/count
metadata and scores. Trusted-local sinks receive the raw query and records.
Slice 181 adds read-touch metadata: successful `Runtime::recall` and
`HybridRuntime::recall` call `Backend::touch(...)` for each returned hit before
rendering framing, so returned hits carry updated `last_read_at`. Ordinary
`Runtime::search` and `HybridRuntime::search` remain read-only and do not touch
records. Slice 182 adds the library-level decay execution boundary:
`DecayRequest` selects one scope, `unused_before`, `importance_floor`, a
positive batch `limit`, and `decay_at`; `Fts5Backend::decay(...)` marks visible
records with `last_read_at < unused_before` and
`importance <= importance_floor` as shadow, advances `updated_at`
monotonically to `decay_at`, syncs FTS shadow metadata, and returns the records
it changed. Default search already excludes those rows, while
`Query::include_shadow=true` keeps them inspectable.
Slice 183 adds the operator-facing retention config contract:
`memory.longterm.retention.forget_after_unused_days`,
`importance_floor`, `max_records_per_scope`, and
`decay_check_interval_hours` parse through `oran-config`, default to
180 days / 0.0 / 10000 / 24 h, reject malformed values, and preserve the
strict/loose unknown-field behavior used by the rest of the nested memory
config surface. Slice 184 consumes that policy at configured-route startup:
`bootstrap::run` derives a `LongtermMemoryStartupDecayOptions` pass for the
runner's `cli` scope, and `RuntimeAssembly::build` applies that bounded
lexical-memory decay after migration and before exposing the long-lived
memory pool. Slice 185 makes that startup pass observable by retaining the
shadowed-record count on `RuntimeAssembly` and printing it in the startup
banner as `startup-decay=<disabled|N>`. Slice 186 publishes that successful
startup pass as advisory `memory_decay` metadata through build-only
`RuntimeAssemblyOptions::startup_hook_bindings`; the payload carries source,
scope, policy inputs, shadowed count, and timing, but no decayed record content.
Slice 187 adds the first `oran-automation` cadence planner: it evaluates the
retention interval from caller-supplied job state and produces a due-only
`memory::longterm::DecayRequest`. Slice 188 adds bootstrap mapping from
configured retention policy into a stored `MemoryRetentionJob` descriptor whose
first fire is after the startup pass. Slice 189 adds
`AutomationRepository` for durable retention job/run/last-fired state above
`storage::Pool`, without making bootstrap open `automation.db`. Periodic
execution and periodic hook publishing remain downstream.
Default builds still reject
`memory.longterm.hybrid_search.enabled=true` before assembly/provider side
effects with `reason=build_option_disabled`, `option=vector_memory`. Semantic or
external embedding providers, blocking read-before policy, periodic decay
ownership, and periodic automation decay publishing remain downstream.

```cpp
// include/oran/memory/longterm.hpp
namespace orangutan::memory::longterm {

enum class RecordKind : std::uint8_t {
  user,
  feedback,
  project,
  reference,
  team,
};

struct RecordKey {
  std::string id;
  std::string scope_key;        // agent identity scope
};

struct Record {
  RecordKey   key;
  RecordKind  kind;
  std::string title;
  std::string body;
  core::Time   created_at;
  core::Time   updated_at;
  core::Time   last_read_at;
  double       importance = 0.0; // 0..1
  std::vector<std::string> tags;
  std::vector<std::string> linked_record_ids;
  bool         shadow = false;
};

struct Query {
  std::string scope_key;
  std::string text;
  std::vector<RecordKind> kinds;
  bool include_shadow = false;
};

struct SearchHit {
  Record record;
  double score = 0.0;
  std::optional<double> lexical_score;
  std::optional<double> vector_score;
};

struct TouchRequest {
  RecordKey key;
  core::Time read_at;
};

struct DecayRequest {
  std::string scope_key;
  core::Time unused_before;
  double importance_floor = 0.0;
  std::size_t limit = 0;
  core::Time decay_at;
};

struct DecayResult {
  std::vector<Record> shadowed_records;
};

}  // namespace orangutan::memory::longterm
```

`Backend` is the seam where alternative implementations plug in:

```cpp
class Backend {
 public:
  virtual ~Backend() = default;
  virtual async::Awaitable<core::Result<Record>> get(RecordKey key) = 0;
  virtual async::Awaitable<core::Result<std::vector<SearchHit>>>
    search(Query query, std::size_t limit) = 0;
  virtual async::Awaitable<core::Result<Record>> upsert(WriteRequest request) = 0;
  virtual async::Awaitable<core::Result<Record>> touch(TouchRequest request) = 0;
  virtual async::Awaitable<core::Result<DecayResult>> decay(DecayRequest request) = 0;
  virtual async::Awaitable<core::Result<void>> remove(RecordKey key) = 0;
};

class VectorBackend {
 public:
  virtual ~VectorBackend() = default;
  virtual async::Awaitable<core::Result<void>> upsert(VectorUpsert request) = 0;
  virtual async::Awaitable<core::Result<std::vector<VectorHit>>>
    search(VectorSearchQuery query, std::size_t limit) = 0;
  virtual async::Awaitable<core::Result<void>> remove(VectorRemoveRequest request) = 0;
};
```

`Runtime` composes a backend into the prompt-boundary recall operation:

```cpp
struct RecallRequest {
  Query query;
  std::size_t limit;
};

struct RecallResult {
  std::vector<SearchHit> hits;
  memory::Framing framing;
};

class Runtime {
 public:
  explicit Runtime(Backend& backend);
  async::Awaitable<core::Result<std::vector<SearchHit>>>
    search(Query query, std::size_t limit);
  async::Awaitable<core::Result<RecallResult>> recall(RecallRequest request);
};
```

`HybridRuntime` composes lexical records with a vector index without making
`Fts5Backend` know about embeddings:

```cpp
struct HybridSearchRequest {
  Query query;
  VectorEmbedding embedding;
  std::size_t lexical_limit;
  std::size_t vector_limit;
  std::size_t result_limit;
  double lexical_weight = 1.0;
  double vector_weight = 1.0;
};

class HybridRuntime {
 public:
  HybridRuntime(Backend& lexical_backend, VectorBackend& vector_backend);
  async::Awaitable<core::Result<std::vector<SearchHit>>>
    search(HybridSearchRequest request);
  async::Awaitable<core::Result<RecallResult>>
    recall(HybridSearchRequest request);
};
```

The shipped bootstrap embedding owner is intentionally small and deterministic:
`make_text_embedding(text, TextEmbeddingOptions{model, dimensions})` lowercases
ASCII text, hashes token features into a fixed-width vector, and L2-normalizes
the result. `make_record_embedding(record, options)` embeds title, body, tags,
and linked ids for `memory.remember` vector mirroring. This owner exists to make
the hybrid runtime path testable and dependency-free beyond sqlite-vec; it is
not a semantic embedding model. External or provider-backed embeddings remain a
separate runtime owner.

Recall framing is deterministic prompt text. It contains no clocks, request ids,
trace ids, or scores; it is a function of the returned memory records only. The
current renderer emits one compact section headed `Long-term memory:` with
record kind, title, id, normalized body text, tags, and linked ids. Empty recall
returns an empty `memory::Framing`, so callers can keep section 5 absent when no
long-term memory matches. `render_recall_data_json(...)` serializes the same
returned hits as `{kind:"memory_recall", match_count, records[]}` for tool
results, including record ids, scope keys, kind spellings, timestamps, scores,
tags, linked ids, and shadow flags. `render_forget_data_json(...)` serializes
scoped deletes as `{kind:"memory_forget", record:{id, scope_key}}`.
Since slice 181, recall first touches every returned record through the lexical
backend and the framing/data JSON reflect the updated `last_read_at`; plain
search callers still get a side-effect-free ranked result.

The shipped backends are:

- `Fts5Backend` — SQLite FTS5; default lexical `Backend`, backed by
  `longterm_records` plus `longterm_records_fts`. The FTS table indexes title,
  body, and tags, while scope, id, kind, and shadow remain structured filters.
  `Fts5BackendOptions::migrations_directory` exists for tests/operators that
  need to load migrations from disk; the default path uses the embedded SQL.
- `SqliteVecBackend` — optional sqlite-vec `VectorBackend`, compiled only when
  xmake configures `--vector_memory=y`. Callers pass
  `SqliteVecBackend::auto_extensions()` when opening the backing
  `storage::Pool`; migration verifies `vec_version()`, creates a scoped `vec0`
  table with `embedding float[N] distance_metric=cosine`, rejects dimension
  drift if an existing table uses a different `N`, and stores one vector row per
  scoped `RecordKey`. Default builds expose the public type but return
  `ErrorKind::config` with `reason=build_option_disabled` from migration and
  vector operations. Configured-route bootstrap uses a separate
  `.orangutan/memory-vectors.db` pool for this backend when
  `memory.longterm.hybrid_search.enabled=true` and the binary is built with
  `--vector_memory=y`.

Planned optional backends:

- external vector adapter — optional embedding store via `oran-http::Client`.

`bench/memory/` (see `docs/product-specs/0010-benchmark-harness.md`) records the
FTS5 10k-record search baseline and, since slice 173, the
FTS5-vs-vector-vs-hybrid comparison (`scenarios/search_fts5_vs_vector.cpp`) over a
brute-force cosine reference `VectorBackend`. Slice 177 extends that scenario
under `--vector_memory=y` so the shipped `SqliteVecBackend` reports the same
corpus: on the local release run, sqlite-vec vector-only search was
**~3.03 ms / batch** and FTS5+sqlite-vec hybrid was **~18.96 ms / batch** at
`limit=10`. Since slice 178, the validated
`memory.longterm.hybrid_search` config block is consumed by configured-route
bootstrap in `--vector_memory=y` builds; default builds still fail fast on
`enabled=true` instead of silently falling back to lexical recall.

## Shared Memory (Team)

Cross-agent notes for a team. Like long-term but namespaced by `team_id` not
`scope_key`. Use cases:

- Leader posts a high-level plan that workers consult.
- A worker records "I've checked X, Y, Z" so siblings don't redo work.
- Shared findings during a research session.

Same API shape as `longterm::Runtime`, separate `Store` and storage table. Permission
gating: only agents that are members of the team can read/write.

## Reading Memory Once Per Turn

A specific lesson from the legacy code: memory queries inside the ReAct loop were
re-issued every iteration. The prompt section did not change across iterations of a
single turn (the user prompt was fixed) but the code recomputed it anyway.

v2 enforces:

```cpp
async::Awaitable<core::Result<RunResult>>
agent::Loop::run(std::string prompt) {
  // Pre-loop: render memory section ONCE.
  auto memory_section = co_await prompt_builder.render_memory(prompt, identity_);
  working_.rendered_memory_section = std::move(memory_section);

  for (std::size_t i = 0; i < max_iterations_; ++i) {
    // Per-iteration: use working_.rendered_memory_section as cached input.
    auto request = prompt_builder.build(working_);
    // ...
  }
}
```

The memory section is rendered once before the loop. The skills catalog is also
rendered from a prompt-boundary snapshot in the configured-route runner: skill file
changes surface on the next prompt, not midway through an active turn.

## Retention Policy

`retention::Policy` is a config-defined record parsed from
`memory.longterm.retention`:

```cpp
struct Policy {
  std::chrono::days   forget_after_unused = std::chrono::days(180);
  double              importance_floor    = 0.0;  // 0..1; <= threshold = candidate for shadow
  std::size_t         max_records_per_scope = 10000;
  std::chrono::hours  decay_check_interval  = std::chrono::hours(24);
};
```

The config spelling uses explicit units:
`forget_after_unused_days`, `importance_floor`, `max_records_per_scope`, and
`decay_check_interval_hours`.

Configured-route startup runs one bounded decay pass from the policy before
prompt/tool reads are exposed. Bootstrap also stores an automation-owned
`MemoryRetentionJob` descriptor for the same policy, with `first_fire_at` set to
startup time plus `decay_check_interval_hours`. `oran-automation` has a
deterministic periodic planner that can use that descriptor to decide whether a
retention job is due and, when due, produce the matching `DecayRequest`; startup
does not loop or schedule background work. Decayed records are not immediately deleted;
they enter a "shadow" state where they are excluded from default search but
visible to runtime callers that set `Query::include_shadow=true`. The shipped
`memory.recall` tool keeps `include_shadow=false`. Slice 181 provides the
read-touch metadata prerequisite (`Backend::touch` plus recall-side
`last_read_at` updates), slice 182 provides the backend execution boundary
(`Backend::decay`) that applies the shadow transition for a bounded scope batch,
slice 183 provides the parsed policy contract, slice 184 provides the
configured-route startup owner, and slice 185 exposes the startup pass shadow
count for diagnostics. Slice 186 publishes successful startup decay as advisory
`memory_decay` metadata. Slice 187 adds the pure `oran-automation` retention
cadence/request planner. Slice 188 maps config into the periodic job descriptor
at bootstrap. Slice 189 persists that descriptor and future run rows in
`automation.db` through `AutomationRepository`. Remaining ownership work is the
explicit automation service/tick owner and the periodic decay producer.

Forgetting is final (DELETE), with an audit row in `audit.db`.

## MEMORY.md Mirror (Optional)

Same idea as the legacy `memory-mirror`: an optional `<workspace>/.orangutan/memory/
MEMORY.md` file kept in sync with the long-term store, for human inspection. v2 makes
this **opt-in per agent**, configured by `agent.<name>.memory.mirror`.

## Hook Surface

Memory lifecycle:

- `memory.read.before(scope, kind, query)` — planned; may rewrite the query.
- `memory.read.after(scope, kind, results)` — shipped in slice 180 as advisory
  after successful prompt-boundary long-term recall and `memory.recall` tool
  reads. Default hook sinks receive source, scope, kind, limit, match count,
  score, timing, hybrid flag, and byte/count metadata; trusted-local sinks also
  receive the raw recall query and hit records.
- `memory.write.before(scope, record)` — shipped for bootstrap
  `memory.remember` in slice 179 as veto/proceed only. The blocking publish runs
  before lexical/vector backend mutation; veto returns permission-denied and
  skips persistence. Rewrite/annotation remain downstream.
- `memory.write.after(scope, record)` — shipped in slice 179 as advisory after
  successful bootstrap `memory.remember`.
- `memory.forget(scope, id)` — shipped in slice 179 as advisory after
  successful bootstrap `memory.forget`.
- `memory.decay(scope, count)` — shipped for the configured-route startup
  retention pass in slice 186 as advisory metadata (`source`, scope, policy
  inputs, shadowed count, timing) with no record content. `oran-automation`
  now plans periodic retention requests, and bootstrap stores the mapped job
  descriptor, but periodic decay publishing remains downstream until a periodic
  producer actually executes decay.

These hooks are why team shared memory works: the orchestration leader can install a
`memory.write.after` hook on the shared tier to mirror notes to a Slack channel, for
example.

## Identity / Scope Derivation

Same scheme as legacy:

- `agent_key` (config-defined; e.g. `coder`, `research`).
- `runtime_key` derived per-process (UUID).
- `scope_key = "<agent_key>:<runtime_key>"` for long-term records *unless* config
  pins `scope_key = "<agent_key>"` (shared across runtimes — useful for stable
  expertise).
- `team_id` derived from team definition.

Identity is opaque outside `oran-bootstrap::Identity`. No one else constructs scope
keys.

## Database Layout

Separate files (the audit identified single-DB contention):

- `<workspace>/.orangutan/sessions.db`
- `<workspace>/.orangutan/memory.db`
- `<workspace>/.orangutan/automation.db` (retention job/run schema owned by
  `oran-automation`; bootstrap does not open it yet)
- `<workspace>/.orangutan/audit.db`

Migrations:

- One `migrations/` dir per DB, numbered `0001-<slug>.sql`, `0002-<slug>.sql`, …
- Default shipped migrations are embedded into the library that owns the schema
  with C++26 `#embed`; filesystem migration loading remains available for
  tests/operator overrides.
- Applied through `oran-storage`'s migration runner and recorded in a
  `schema_versions` table per DB.

## Anti-Goals

- We do **not** try to build a vector DB from scratch. If vector search is wanted,
  pick an existing component (sqlite-vec / external API) and plug it in.
- We do **not** try to make memory cross-runtime by default. Multi-runtime memory
  sharing is an explicit deployment decision (set a stable `scope_key`).
- Memory is **not** a fallback storage for arbitrary state. Anything that is not a
  fact about the world goes in session memory, not long-term.

## See Also

- [`permissions-and-hooks.md`](permissions-and-hooks.md)
- [`secrets-and-state.md`](secrets-and-state.md) — DB file layout, migrations.
- [`../product-specs/0005-memory-system.md`](../product-specs/0005-memory-system.md)
  — concrete v1 deliverables.

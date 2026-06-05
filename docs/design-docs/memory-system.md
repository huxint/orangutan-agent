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
from recalled long-term records before the loop starts. `AgentPromptRunner`
copies that rendered string into `RunTurnInputs::memory_framing`, so
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
- **Decay policy**: `memory-age` style decay is actually wired into the search pipeline
  this time; expired records receive lower BM25 weight before potentially being pruned.

Status (slice 164): `include/oran/memory/longterm.hpp` now ships the public
record/query/write shapes, reflection-backed `RecordKind`, `Backend` and
`VectorBackend` traits, validation helpers for record keys, search limits,
record metadata, and vector embeddings, plus `Fts5Backend` as the default
SQLite FTS5 lexical backend. `Fts5Backend` owns the long-term memory schema in
`src/oran-memory/migrations/longterm/`, applies its built-in migration through
the shared `storage::Pool` writer, returns `ErrorKind::not_found` for missing
`get(...)` rows, and treats `remove(...)` as idempotent. Slice 162 adds
`longterm::Runtime`, a prompt-boundary composition layer that delegates search
to a `Backend`, validates recall requests before dispatch, and renders stable
`memory::Framing` bytes from returned hits with `render_recall_framing(...)`.
Slice 163 wires `RuntimeAssembly` to own a separate
`<workspace>/.orangutan/memory.db` pool, migrate `Fts5Backend`, expose
`longterm_memory_backend()` / `longterm_memory_runtime()`, and enable that state
only for configured-route bootstrap startup. Slice 164 adds an explicit
`AgentPromptRunnerOptions::longterm_recall` opt-in that queries that runtime once
at the prompt boundary and feeds the resulting deterministic framing into section
5 before `agent::Loop`. It does **not** yet add config policy for recall queries,
enable recall from ordinary `bootstrap::run`, ship the gated sqlite-vec adapter,
or perform hybrid ranking.

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

Recall framing is deterministic prompt text. It contains no clocks, request ids,
trace ids, or scores; it is a function of the returned memory records only. The
current renderer emits one compact section headed `Long-term memory:` with
record kind, title, id, normalized body text, tags, and linked ids. Empty recall
returns an empty `memory::Framing`, so callers can keep section 5 absent when no
long-term memory matches.

The shipped lexical backend is:

- `Fts5Backend` — SQLite FTS5; default lexical `Backend`, backed by
  `longterm_records` plus `longterm_records_fts`. The FTS table indexes title,
  body, and tags, while scope, id, kind, and shadow remain structured filters.
  `Fts5BackendOptions::migrations_directory` exists for tests/operators that
  need to load migrations from disk; the default path uses the embedded SQL.

Planned optional backends:

- sqlite-vec adapter — optional under `--vector_memory=y`; future search hybrid
  combines FTS5 score + vector cosine.
- external vector adapter — optional embedding store via `oran-http::Client`.

`bench/oran-memory/` (see `docs/product-specs/0010-benchmark-harness.md`) will compare
backends on a synthetic 10k-record corpus.

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

`retention::Policy` is a config-defined record:

```cpp
struct Policy {
  std::chrono::days   forget_after_unused = std::chrono::days(180);
  double              importance_floor    = 0.0;  // 0..1; below = candidate for prune
  std::size_t         max_records_per_scope = 10000;
  std::chrono::hours  decay_check_interval  = std::chrono::hours(24);
};
```

A periodic job (registered with `oran-automation`) runs decay according to the policy.
Decayed records are not immediately deleted; they enter a "shadow" state where they
are excluded from default search but visible via `memory.recall("...", include_shadow=true)`.

Forgetting is final (DELETE), with an audit row in `audit.db`.

## MEMORY.md Mirror (Optional)

Same idea as the legacy `memory-mirror`: an optional `<workspace>/.orangutan/memory/
MEMORY.md` file kept in sync with the long-term store, for human inspection. v2 makes
this **opt-in per agent**, configured by `agent.<name>.memory.mirror`.

## Hook Surface

Memory lifecycle:

- `memory.read.before(scope, kind, query)` — may rewrite the query.
- `memory.read.after(scope, kind, results)` — observability.
- `memory.write.before(scope, record)` — may veto, rewrite, or annotate.
- `memory.write.after(scope, record)`
- `memory.forget(scope, id)`
- `memory.decay(scope, count)` — periodic.

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
- `<workspace>/.orangutan/automation.db`
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

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
  `agent::Loop`, leaving long-term recall/search out of v1 while preserving the
  once-per-turn boundary.
- Long-term backend contract prework shipped in slice 160:
  `memory::longterm::RecordKind` (`user`, `feedback`, `project`, `reference`,
  reserved `team`), `RecordKey`, `Record`, `Query`, `SearchHit`,
  `WriteRequest`, `Backend`, `VectorBackend`, and validation helpers now exist
  in `<oran/memory/longterm.hpp>`.
- SQLite FTS5 long-term backend shipped in slice 161:
  `memory::longterm::Fts5Backend` owns the default lexical schema under
  `src/oran-memory/migrations/longterm/`, migrates it through `storage::Pool`,
  implements scoped `get` / `search` / `upsert` / idempotent `remove`, filters
  by kind and shadow state, and returns lexical scores from SQLite BM25.
  `longterm::Runtime`, vector search, hybrid ranking, and recall-backed framing
  remain downstream.
- Decay policy applied by a periodic job (`oran-automation`).
- Optional `MEMORY.md` mirror under `<workspace>/.orangutan/memory/`.
- Hook events on read / write / forget / decay.

## Scope (v1.1)

- `Memory::team::Store` — shared tier for `oran-orchestration` teams.
- `kind = team` for cross-tier search visibility.
- Approval signing replay-safe across rotations.

## Scope (v2)

- Gated sqlite-vec adapter under `--vector_memory=y` implementing the shipped
  `memory::longterm::VectorBackend` contract.
- Hybrid search (FTS5 score + cosine).
- Externalized embedding store via `oran-http::Client`.

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
   a 10 k-record corpus. **Status:** open; slice 161 ships the default FTS5
   lexical `Backend`, but the runtime composition layer and 10 k-record bench
   are still downstream.
3. The MEMORY.md mirror, when enabled, reflects all kinds + records within 1 s of the
   underlying DB write.
4. Decay marks records older than `policy.forget_after_unused` as shadow; they no
   longer surface in default search.
5. A `memory.write.before` hook can veto a write; the runtime returns
   `Error::HookVeto` to the caller.
6. `tests/memory/` >= 85% coverage. **Status:** `test-memory` currently reports
   20 cases / 678 assertions, including long-term contract validation, fake
   async backend interface coverage, and public `Fts5Backend` migration /
   scoped search / filtering / update / delete coverage.
7. `bench/memory/search-fts5-vs-vector` (v2): reports the FTS5 baseline + vector
   results in machine-readable JSON.

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
xmake run test-bootstrap
scripts/bench-compare.sh memory
```

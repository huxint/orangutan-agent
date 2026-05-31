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
- `oran-memory::longterm::Runtime` with `Fts5Backend` (default).
- `MemoryRecord` kinds: `user`, `feedback`, `project`, `reference`.
- Decay policy applied by a periodic job (`oran-automation`).
- Optional `MEMORY.md` mirror under `<workspace>/.orangutan/memory/`.
- Hook events on read / write / forget / decay.

## Scope (v1.1)

- `Memory::team::Store` — shared tier for `oran-orchestration` teams.
- `kind = team` for cross-tier search visibility.
- Approval signing replay-safe across rotations.

## Scope (v2)

- `VectorBackend` (optional, gated `--vector_memory=y`).
- Hybrid search (FTS5 score + cosine).
- Externalized embedding store via `oran-http::Client`.

## Out Of Scope

- Cross-runtime sync without explicit `scope_pin`.
- Memory replication.

## Acceptance Criteria

1. A `SessionStore::append(...)` followed by `SessionStore::load(...)` round-trips
   500 messages without loss. **Status:** closed for
   `memory::session::Store` by `test-memory` slice 130 coverage; slice 131 wires
   the runtime assembly owner. Bootstrap runner persistence is the next wiring
   step.
2. `longterm::Runtime::search("react agent loop", limit=10)` returns within 50 ms on
   a 10 k-record corpus.
3. The MEMORY.md mirror, when enabled, reflects all kinds + records within 1 s of the
   underlying DB write.
4. Decay marks records older than `policy.forget_after_unused` as shadow; they no
   longer surface in default search.
5. A `memory.write.before` hook can veto a write; the runtime returns
   `Error::HookVeto` to the caller.
6. `tests/memory/` ≥ 85% coverage.
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

# `bench/memory`

Memory benchmarks cover the typed memory-layer overhead above storage
repositories, the long-term memory search baseline, and the
FTS5-vs-vector-vs-hybrid long-term search comparison.

| Scenario | Compares |
| --- | --- |
| [`scenarios/session_store.cpp`](scenarios/session_store.cpp) | Raw `storage::SessionRepository` append/load of one 64-message batch vs. typed `memory::session::Store` append/load of the same logical conversation. |
| [`scenarios/longterm_fts5.cpp`](scenarios/longterm_fts5.cpp) | `memory::longterm::Runtime::search` through the default FTS5 backend over a seeded 10k-record corpus at `limit=10`; this is the baseline future sqlite-vec / hybrid ranking slices must compare against. |
| [`scenarios/search_fts5_vs_vector.cpp`](scenarios/search_fts5_vs_vector.cpp) | FTS5 lexical `Runtime::search` vs. an in-bench brute-force cosine `VectorBackend` vs. `memory::longterm::HybridRuntime::search`, all over one shared seeded 10k-record corpus at `limit=10`. When built with `--vector_memory=y`, the same scenario also reports sqlite-vec vector-only and FTS5+sqlite-vec hybrid rows through the shipped `SqliteVecBackend` contract. |

Both long-term search scenarios measure search only: corpus migration, record
seeding, and embedding generation run once before nanobench starts timing. The
cosine `VectorBackend` in `search_fts5_vs_vector.cpp` is an in-process reference
implementation; the gated `--vector_memory=y` sqlite-vec backend uses a separate
temporary vector DB with the same deterministic record keys and embeddings so the
default FTS5 pool does not require extension registration.

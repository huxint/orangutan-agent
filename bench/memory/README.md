# `bench/memory`

Memory benchmarks cover the typed memory-layer overhead above storage
repositories, the long-term memory search baseline, and the
FTS5-vs-vector-vs-hybrid long-term search comparison.

| Scenario | Compares |
| --- | --- |
| [`scenarios/session_store.cpp`](scenarios/session_store.cpp) | Raw `storage::SessionRepository` append/load of one 64-message batch vs. typed `memory::session::Store` append/load of the same logical conversation. |
| [`scenarios/longterm_fts5.cpp`](scenarios/longterm_fts5.cpp) | `memory::longterm::Runtime::search` through the default FTS5 backend over a seeded 10k-record corpus at `limit=10`; this is the baseline future sqlite-vec / hybrid ranking slices must compare against. |
| [`scenarios/search_fts5_vs_vector.cpp`](scenarios/search_fts5_vs_vector.cpp) | FTS5 lexical `Runtime::search` vs. an in-bench brute-force cosine `VectorBackend` vs. `memory::longterm::HybridRuntime::search`, all over one shared seeded 10k-record corpus at `limit=10`. First scenario exercising the slice-172 `HybridRuntime`; the cosine backend is a reference implementation until the gated `--vector_memory=y` sqlite-vec adapter satisfies the same `VectorBackend` contract. |

Both long-term search scenarios measure search only: corpus migration, record
seeding, and embedding generation run once before nanobench starts timing. The
cosine `VectorBackend` in `search_fts5_vs_vector.cpp` is an in-process reference
implementation; the gated `--vector_memory=y` sqlite-vec backend will satisfy the
same `memory::longterm::VectorBackend` contract and re-run this comparison.

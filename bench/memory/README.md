# `bench/memory`

Memory benchmarks cover the typed memory-layer overhead above storage
repositories and the long-term memory search baseline.

| Scenario | Compares |
| --- | --- |
| [`scenarios/session_store.cpp`](scenarios/session_store.cpp) | Raw `storage::SessionRepository` append/load of one 64-message batch vs. typed `memory::session::Store` append/load of the same logical conversation. |
| [`scenarios/longterm_fts5.cpp`](scenarios/longterm_fts5.cpp) | `memory::longterm::Runtime::search` through the default FTS5 backend over a seeded 10k-record corpus at `limit=10`; this is the baseline future sqlite-vec / hybrid ranking slices must compare against. |

The FTS5 scenario intentionally measures search only: corpus migration and seeding
run once before nanobench starts timing. Vector and hybrid scenarios land with
the gated `--vector_memory=y` backend.

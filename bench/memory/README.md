# `bench/memory`

Memory benchmarks cover the typed memory-layer overhead above storage
repositories.

| Scenario | Compares |
| --- | --- |
| [`scenarios/session_store.cpp`](scenarios/session_store.cpp) | Raw `storage::SessionRepository` append/load of one 64-message batch vs. typed `memory::session::Store` append/load of the same logical conversation. |

The first scenario is intentionally session-focused. Long-term FTS5/vector search
benchmarks land with the long-term backend slice, not with the session wrapper.

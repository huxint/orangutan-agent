# Third-Party Libraries

`xmake/packages.lua` owns version selection. Add a dependency only when existing
code cannot satisfy a concrete requirement; record its purpose, license, boundary
and measured or estimated compile cost here before adding it to the build.

| Library | Version | Consumer and purpose | License | Boundary / cost |
| --- | --- | --- | --- | --- |
| `asio` | 1.36.0 | Async executors, coroutines and cancellation | BSL-1.0 | Executor handles may be public; implementation headers have moderate cost. |
| `libcurl` | >=8.11.0, system | HTTP/SSE transport | curl | Private to `oran-http`; moderate cost. |
| `nlohmann_json` | 3.12.0 | Config, tool, provider, agent, memory and bootstrap JSON | MIT | Implementation files only; moderate cost. |
| `re2` | 2025.11.05 | Configuration validation and permission input patterns | BSD-3-Clause | Private compiled regex owner; moderate cost. |
| `sqlite3` | 3.51.0+0 | Storage with FTS5 lexical memory | Public domain | C API private to `oran-storage`; moderate cost. |
| `libsodium` | 1.0.21 | Approval authentication and random keys | ISC | Private to `oran-permission`; low cost. |
| `catch2` | 3.7.1 | Behavioral tests | BSL-1.0 | Test targets only; moderate cost. |
| `nanobench` | 4.3.11 | Benchmarks | MIT | Benchmark targets only; low cost. |

Use one library per infrastructure responsibility. Keep optional packages out of
default builds. Version changes update this table and receive the same boundary
review as an initial dependency. `make ci` checks manifest/document agreement.

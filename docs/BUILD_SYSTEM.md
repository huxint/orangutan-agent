# Build System

The supported build uses C++26, GCC 16.1+ and xmake 3.1.1 on Linux. Current local
validation uses GCC 16.2.1. Source reflection requires this compiler baseline.

## Commands

```sh
xmake f -y -m release
xmake build -j4
xmake test -j4

xmake build test-bootstrap
xmake run test-bootstrap

xmake f -y -m debug --sanitizers=y
xmake build -j4
xmake test -j4
```

Tests and benchmarks are separate targets: `test-<lib>` and `bench-<lib>`.
`xmake test` builds test targets before running them. For a single Catch2 case,
invoke `build/linux/x86_64/release/test-<lib> "case name"` after building it.
Use the debug path when configured in debug mode.

## Build Ownership

| File | Owns |
| --- | --- |
| `xmake.lua` | Language, common warnings and included build definitions. |
| `xmake/targets.lua` | Library graph and the `orangutan` executable. |
| `xmake/packages.lua` | Pinned fetched packages and required system libraries. |
| `xmake/toolchain.lua` | Compiler flags, release LTO and debug sanitizers. |
| `xmake/options.lua` | Supported configure options. |
| `xmake/tests.lua`, `xmake/bench.lua` | Test and benchmark buckets. |

`lto` defaults on in release. `hardened`, `analyze` and `sanitizers` are opt-in;
ASan/UBSan applies only in debug mode. These flags depend on the custom toolchain
being active; the current default selection does not apply them. Explicit
`oran-gcc` selection also needs assembler provisioning for package builds. Both
are [tracked](exec-plans/tech-debt-tracker.md); verify actual compiler/linker
arguments before claiming LTO or sanitizer coverage.

`vector_memory` enables the existing
sqlite-vec backend for library callers. `modules` is experimental; no module
migration has been accepted. No GUI or messaging SDK is required.

System libcurl development headers and pkg-config must be installed before
configuration. Xmake supplies Asio, Catch2, libsodium, nanobench, nlohmann_json,
RE2 and SQLite with FTS5. Exact versions and boundaries live in
[libraries](rules/libraries.md).

## Compile Cost

Public headers keep heavy third-party definitions private. Libraries use the
stable PCH in `include/oran/_pch.hpp`; changing a .cpp should rebuild its owning
archive and dependents' links. Optional packages must not affect default builds.

[compile-budget](rules/compile-budget.md) owns measurement thresholds;
[FAST_COMPILATION](FAST_COMPILATION.md) owns the measurement workflow. Hosted
functional builds are independent of unprovisioned reference-hardware gates.

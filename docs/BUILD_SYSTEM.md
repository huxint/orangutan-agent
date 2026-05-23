# Build System

Orangutan v2 uses **xmake** with **GCC 16.1** as the primary toolchain, compiling
the project as **C++26**. This file captures the build philosophy; mechanical
compile-time discipline lives in
[`FAST_COMPILATION.md`](FAST_COMPILATION.md) and [`rules/compile-budget.md`](rules/compile-budget.md).

> Why xmake (kept): the legacy project already used xmake, the team is fluent in it,
> and per-target packaging is clean.
>
> Why GCC 16.1 + C++26: by the v2 timeframe GCC 16.1 ships stable `-std=c++26`,
> `<print>`, mature `std::expected`, `std::generator`, deducing-`this`, `std::span`,
> the C++26 P2300 utilities, and the HALO improvements that matter for our coroutine
> surface. Clang ≥ 19 is supported as a secondary toolchain.

## Toolchain Requirements

```text
Primary:
  - GCC 16.1+
  - xmake 2.9+
  - libsodium development files (system or fetched by xmake)
  - libcurl development files (system) for transport
  - nlohmann_json source (fetched by xmake)
  - sqlite3 source (fetched by xmake)

Secondary (CI matrix):
  - Clang 19+
  - musl-clang for static builds (stretch)
```

Disable any toolchain combination at configure time:

```sh
xmake f --toolchain=gcc       # default
xmake f --toolchain=clang
xmake f --runtime=c++_static  # static c++ runtime (stretch)
```

## Top-Level xmake Files

```
xmake.lua                # project-wide settings, includes the rest
xmake/options.lua        # CLI options (channels, sanitizers, modules, etc.)
xmake/toolchain.lua      # GCC 16.1 flags + module setup
xmake/packages.lua       # third-party requirements
xmake/targets.lua        # one entry per library + binary
xmake/tests.lua          # one entry per tests/<lib>/ bucket
xmake/bench.lua          # one entry per bench/<lib>/ bucket
xmake/checks.lua         # CI-friendly checks (deps, includes, TU times)
```

## Project-Wide Settings

```lua
-- xmake.lua
set_project("orangutan-v2")
set_version("2.0.0")
set_languages("c++26")           -- GCC 16.1 ships -std=c++26 stable
set_warnings("all", "extra")
add_rules("mode.debug", "mode.release", "mode.releasedbg")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".", lsp = "clangd"})
set_policy("package.requires_lock", true)
set_policy("build.warning", true)            -- show compiler warning commands

includes("xmake/options.lua")
includes("xmake/toolchain.lua")
includes("xmake/packages.lua")
includes("xmake/targets.lua")
includes("xmake/tests.lua")
includes("xmake/bench.lua")
includes("xmake/checks.lua")
```

## Toolchain Configuration

```lua
-- xmake/toolchain.lua
toolchain("oran-gcc")
    set_homepage("GCC 16.1 with our flags")
    set_kind("standalone")
    -- Probe `gcc-16` / `g++-16` first (Debian-style suffixed names); fall back to
    -- the unsuffixed `gcc` / `g++` (the case on this dev machine, where GCC 16.1.1
    -- is the system compiler and there is no `-16` symlink).
    local cc  = find_program and (find_program("gcc-16") or find_program("gcc")) or "gcc"
    local cxx = find_program and (find_program("g++-16") or find_program("g++")) or "g++"
    set_toolset("cc",  cc)
    set_toolset("cxx", cxx)
    set_toolset("ld",  cxx)
    on_load(function (toolchain)
        toolchain:add("cxxflags",
            "-std=c++26",
            "-fdiagnostics-color=always",
            "-pipe",
            "-fdiagnostics-show-template-tree",
            "-fno-plt",
            "-fmacro-prefix-map=" .. os.projectdir() .. "=.",
            "-fno-common"
        )
        if has_config("modules") then
            toolchain:add("cxxflags", "-fmodules")
        end
        if has_config("lto") then
            toolchain:add("cxxflags", "-flto=auto")
            toolchain:add("ldflags", "-flto=auto")
        end
        if has_config("hardened") then
            toolchain:add("cxxflags",
                "-D_FORTIFY_SOURCE=3",
                "-fstack-protector-strong",
                "-fcf-protection",
                "-fstack-clash-protection"
            )
        end
        if has_config("analyze") then
            toolchain:add("cxxflags",
                "-fanalyzer",
                "-Werror=analyzer-null-dereference",
                "-Werror=analyzer-use-after-free",
                "-Werror=analyzer-double-free",
                "-Werror=analyzer-malloc-leak",
                "-Werror=analyzer-tainted-allocation-size",
                "-Werror=analyzer-tainted-array-index",
                "-Werror=analyzer-out-of-bounds",
                "-Werror=analyzer-write-to-string-literal",
                "-Werror=analyzer-fd-leak",
                "-Werror=analyzer-fd-use-without-check"
            )
        end
    end)
toolchain_end()
```

See [`rules/static-analysis.md`](rules/static-analysis.md) for the `-fanalyzer`
policy, the required warning set, and the suppression rules.

## Options

```lua
-- xmake/options.lua
option("modules")
    set_default(false)
    set_description("Enable C++26 modules (off until a module slice lands)")
option_end()

option("lto")
    set_default(true)
    set_description("Enable LTO in release mode")
option_end()

option("hardened")
    set_default(false)
    set_description("Enable hardening flags")
option_end()

option("sanitizers")
    set_default(false)
    set_description("Enable ASan/UBSan in debug builds")
option_end()

option("analyze")
    set_default(false)
    set_description("Enable GCC 16.1 -fanalyzer (see rules/static-analysis.md)")
option_end()
```

Feature options such as `channel_qq`, `hook_lua`, and `vector_memory` land with the
library that consumes them.

## Packages

```lua
-- xmake/packages.lua
add_requires("asio 1.36.0")
add_requires("catch2 3.7.1")
add_requires("nanobench 4.3.11")
add_requires("nlohmann_json 3.12.0")
add_requires("sqlite3 3.51.0+0")
```

Packages land with the library that first consumes them. The full approval list
and planned versions live in [`rules/libraries.md`](rules/libraries.md); for example
`fmt`, `spdlog`, and `libcurl` are approved but not required by the current
checked-in targets yet.

**Notable removals vs. legacy:**

- ❌ `stdexec-gtc` (custom NVIDIA fork) — replaced by `asio` + coroutines.
- ❌ `mbedtls` — replaced by `libsodium` for secret crypto; TLS goes through OpenSSL via libcurl.
- ❌ `ctre` — replaced by `re2` (runtime patterns; smaller TU).
- ❌ `uni_algo` — folded into `oran-core::str` using stdlib + simdutf for the bits we need.

## Library Targets

Each shipped library follows the same shape. Future libraries from
`docs/ARCHITECTURE.md` are added here one slice at a time:

```lua
-- xmake/targets.lua
local root = os.projectdir()

local function oran_lib(name, deps, private_packages, public_packages)
    target("oran-" .. name)
        set_kind("static")
        set_group("oran-libs")
        add_includedirs(path.join(root, "include"), { public = true })
        add_files(path.join(root, "src", "oran-" .. name, "**.cpp"))
        if deps then add_deps(table.unpack(deps)) end
        if private_packages then add_packages(table.unpack(private_packages), { public = false }) end
        if public_packages then add_packages(table.unpack(public_packages), { public = true }) end
        set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
end

oran_lib("core", {}, {})
oran_lib("async", { "oran-core" }, {}, { "asio" })
oran_lib("io", { "oran-core", "oran-async" }, {}, { "asio" })
oran_lib("storage", { "oran-core", "oran-async" }, { "sqlite3" })
oran_lib("config", { "oran-core", "oran-storage" }, { "nlohmann_json", "re2" })
oran_lib("permission", { "oran-core", "oran-config", "oran-storage", "oran-async" }, { "re2", "libsodium" })
oran_lib("hook", { "oran-core", "oran-async" }, {})
oran_lib("tool", { "oran-core", "oran-async", "oran-io", "oran-permission", "oran-hook" }, { "nlohmann_json" })
oran_lib("prompt", { "oran-core", "oran-async", "oran-config", "oran-tool" }, {})
oran_lib("provider", { "oran-core", "oran-async", "oran-prompt" }, {})
oran_lib("agent", { "oran-core", "oran-async", "oran-prompt", "oran-tool", "oran-provider" }, { "nlohmann_json" })
oran_lib("cli", { "oran-core" }, {})
oran_lib("bootstrap", { "oran-core", "oran-async", "oran-io", "oran-storage", "oran-config", "oran-permission", "oran-hook", "oran-tool", "oran-cli" }, {})

target("orangutan")
    set_kind("binary")
    add_deps("oran-core", "oran-async", "oran-io", "oran-storage", "oran-config", "oran-permission", "oran-hook", "oran-tool", "oran-cli", "oran-bootstrap")
    add_files(path.join(root, "src/main.cpp"))
    set_rundir(root)
```

`oran-provider` is currently the provider-domain + fake-provider library. It
depends on `oran-async` for `FakeProvider`'s cancel-aware scripted latency and
on `oran-prompt` for adapter-side `prompt::RenderedPrompt` cache-hint mapping.
It is registered with `test-provider` and `bench-provider`, but no transport,
protocol adapter, or real vendor runtime is linked into the binary yet.

`oran-agent` now owns the slice-72 `SessionState` promotion owner plus the
slice-77 sequential direct-dispatch `Loop` turn driver with cancellation-phase
context on provider/tool parent cancellations. The provider
dependency is
intentional and downward: the agent runtime layer drives `provider::System`,
while `oran-provider` never calls back into `oran-agent`. The `orangutan`
binary still does not link `oran-agent`; CLI/binary handoff waits for the
turn-level trace/audit and approval-observability envelope.

**Key compile-time wins from this shape:**

- Each library is its own static archive; touching `oran-channel-qq` does not recompile
  `oran-tool`.
- The PCH (`include/oran/_pch.hpp`) is shared across all libraries; cost is paid once.
- Public headers (`include/oran/<lib>/`) are stable; impl-private headers in
  `src/oran-<lib>/` change freely without ripples.

## PCH

`include/oran/_pch.hpp` includes only stable, low-cost headers:

```cpp
#pragma once

// Stdlib stable headers
#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Project-wide foundations (intentionally small)
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
```

Things deliberately **excluded** from the PCH:

- `<asio.hpp>` — too heavy; `oran-async` adds it locally.
- `<spdlog/spdlog.h>` — too heavy; `oran-log` adds it locally.
- `<fmt/core.h>` — lands with `oran-log`.
- `<nlohmann/json_fwd.hpp>` — lands with the first JSON-owning library.
- `<httplib.h>` — only `oran-web` needs it.
- `<sqlite3.h>` — only `oran-storage` needs it.
- `<regex>` — use `re2` behind the owning library instead.

Per-target PCH override is allowed only if measurement proves > 1s saving (record in
`docs/exec-plans/tech-debt-tracker.md`).

## Modules

GCC 16.1 supports C++20 modules. We adopt them **incrementally**:

1. Phase 1 (MVP): traditional headers + PCH. Modules opt-in via `--modules=y` for
   experiments.
2. Phase 2: `oran-core` and `oran-async` migrate to module units (`*.cppm`).
3. Phase 3: every library migrates.

See [`rules/module-and-pch.md`](rules/module-and-pch.md) for the migration recipe and
fallback when modules misbehave.

## Build Commands

```sh
# Configure
xmake f -m release                 # release
xmake f -m debug --sanitizers=y    # debug + ASan/UBSan
xmake f -m release --modules=y     # release + modules
xmake f -m release --analyze=y     # release + GCC -fanalyzer

# Build the whole project
xmake -j$(nproc)

# Build only one library
xmake build oran-tool

# Run tests for one library
xmake build test-tool && xmake run test-tool

# Run a benchmark
xmake build bench-memory && xmake run bench-memory

# Cross-implementation bench compare
scripts/bench-compare.sh memory
```

## Compile-Time Targets

A clean release build on an 8-core/16 GB machine targets:

| Stage             | Target time | Hard cap |
| ----------------- | ----------- | -------- |
| Configure         | ≤ 5 s       | 20 s     |
| Build all libs    | ≤ 25 s      | 45 s     |
| Link binaries     | ≤ 5 s       | 15 s     |
| Incremental rebuild after 1 .cpp edit | ≤ 3 s | 10 s |

Hard cap exceedance fails CI. See [`rules/compile-budget.md`](rules/compile-budget.md).

## Sanitizers

In debug + `--sanitizers=y`, ASan + UBSan are enabled. TSan is opt-in via
`--sanitizers=tsan` because it conflicts with ASan and adds significant overhead.

## Release Stripping

Release binaries are stripped by xmake's default (`set_strip("all")` per target).
A separate `.debug` artifact is produced for distribution; CI uploads it.

## Static Linking

Optional: `--static=y` produces a fully-static binary via musl-clang (stretch). Useful
for distributing the bench runner.

## Reproducibility

- Package versions are declared in `xmake/packages.lua` and documented in
  [`rules/libraries.md`](rules/libraries.md); `xmake-requires.lock` is ignored
  as a per-machine resolver artifact until CI owns a stable refresh/check flow.
- `-fmacro-prefix-map` strips the build dir from `__FILE__` strings.
- `SOURCE_DATE_EPOCH` honored if set (CI sets it from the commit timestamp).

## Anti-Patterns

- Adding a `.cpp` to a library without checking it compiles in budget.
- Adding a heavy include to a public header.
- Pulling a new third-party package without an entry in
  [`rules/libraries.md`](rules/libraries.md).
- Disabling LTO "for now" without an issue tracking when it's re-enabled.

## See Also

- [`FAST_COMPILATION.md`](FAST_COMPILATION.md) — the engineering playbook.
- [`rules/compile-budget.md`](rules/compile-budget.md) — the enforced budget.
- [`rules/module-and-pch.md`](rules/module-and-pch.md) — PCH/module mechanics.
- [`rules/libraries.md`](rules/libraries.md) — library approval list.

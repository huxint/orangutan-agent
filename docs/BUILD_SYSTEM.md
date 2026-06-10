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
library that consumes them. Shipped so far: `vector_memory` (sqlite-vec memory
backend) and `channel_qq` (slice 229 — gates the `oran-channel-qq` /
`test-channel-qq` / `bench-channel-qq` targets in `xmake/targets.lua`,
`xmake/tests.lua`, and `xmake/bench.lua`; default off until the QQ port's
round-trip acceptance passes, per
`docs/exec-plans/active/2026-06-10-channel-qq-port.md`).

## Packages

```lua
-- xmake/packages.lua
add_requires("asio 1.36.0")
add_requires("catch2 3.7.1")
add_requires("libcurl >=8.11.0", { system = true })
add_requires("libsodium 1.0.21")
add_requires("nanobench 4.3.11")
add_requires("nlohmann_json 3.12.0")
add_requires("re2 2025.11.05")
add_requires("sqlite3 3.51.0+0", { configs = { cflags = "-DSQLITE_ENABLE_FTS5" } })
if has_config("vector_memory") then
    add_requires("sqlite-vec 0.1.9")
end
```

Packages land with the library that first consumes them. The full approval list
and planned versions live in [`rules/libraries.md`](rules/libraries.md).
`libcurl` is consumed privately by `oran-http` through the system package
provider, and `nlohmann_json` is consumed privately by JSON-owning implementation
files in `oran-config`, `oran-tool`, `oran-provider`, `oran-agent`, and
`oran-memory`; public headers still expose bytes and stdlib value types only.
The SQLite package is built with `SQLITE_ENABLE_FTS5` because
`memory::longterm::Fts5Backend` is the default lexical long-term memory backend.
`--vector_memory=y` additionally pulls `sqlite-vec 0.1.9` into `oran-memory` and
publishes `ORAN_ENABLE_SQLITE_VEC`; default builds do not resolve or link that
optional package.

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
oran_lib("http", { "oran-core", "oran-async" }, { "libcurl" }, { "asio" })
oran_lib("io", { "oran-core", "oran-async" }, {}, { "asio" })
oran_lib("storage", { "oran-core", "oran-async" }, { "sqlite3" })
oran_lib("config", { "oran-core", "oran-storage" }, { "nlohmann_json", "re2" })
oran_lib("permission", { "oran-core", "oran-config", "oran-storage", "oran-async" }, { "re2", "libsodium" })
oran_lib("hook", { "oran-core", "oran-async" }, {})
oran_lib("memory", { "oran-core", "oran-async", "oran-storage" }, { "nlohmann_json" })
oran_lib("automation", { "oran-core", "oran-async", "oran-storage", "oran-memory", "oran-hook" }, {})
oran_lib("skill", { "oran-core", "oran-async", "oran-io" }, {})
oran_lib("tool", { "oran-core", "oran-async", "oran-io", "oran-permission", "oran-hook" }, { "nlohmann_json" })
oran_lib("prompt", { "oran-core", "oran-async", "oran-config", "oran-tool" }, {})
oran_lib("provider", { "oran-core", "oran-async", "oran-config", "oran-prompt" }, { "nlohmann_json" })
oran_lib("agent", { "oran-core", "oran-async", "oran-storage", "oran-prompt", "oran-tool", "oran-provider", "oran-hook" }, { "nlohmann_json" })
oran_lib("cli", { "oran-core", "oran-async", "oran-hook", "oran-provider" }, {})
oran_lib("bootstrap", { "oran-core", "oran-async", "oran-http", "oran-io", "oran-storage", "oran-config", "oran-permission", "oran-hook", "oran-memory", "oran-automation", "oran-skill", "oran-tool", "oran-provider", "oran-agent", "oran-cli" }, {})

target("orangutan")
    set_kind("binary")
    add_deps("oran-core", "oran-async", "oran-http", "oran-io", "oran-storage", "oran-config", "oran-permission", "oran-hook", "oran-memory", "oran-automation", "oran-skill", "oran-tool", "oran-provider", "oran-agent", "oran-cli", "oran-bootstrap")
    add_files(path.join(root, "src/main.cpp"))
    set_rundir(root)
```

`oran-http` is the platform HTTP/TLS client target. Slice 110 ships a
body-response `http::Client` over private libcurl handles; callers pass a
blocking executor, so production bootstrap can use `async::Runtime::cpu_executor()`
instead of blocking the main coroutine executor. Slice 111 makes `oran-bootstrap`
depend on `oran-http` for the explicit `HttpProviderBackend` construction seam;
slices 121-123 add the SSE parser/client path used by provider streaming.

`oran-skill` is the section-4 prompt catalog renderer, owner, and markdown
snapshot loader. It depends on `oran-core` for the error/result contract,
`oran-async` for awaitable loader calls, and `oran-io` for policy-free
read/list helpers plus file-view cache invalidation when watcher/signature
refresh reloads changed skill markdown. Slice 138 adds
`skill::WorkspaceSkillSnapshot`, which uses Linux inotify when available and a
bounded content-aware directory signature before prompt-boundary reloads.

`oran-provider` is currently the provider-domain + fake-provider +
execution-runtime + route-resolution + protocol-mapping library. It depends on
`oran-async` for `FakeProvider`'s cancel-aware scripted latency, on
`oran-config` for `provider::resolve_route(Config, route_name)`, and on
`oran-prompt` for adapter-side `prompt::RenderedPrompt` cache-hint mapping. It
is registered with `test-provider` and `bench-provider`. Real HTTP/TLS I/O stays
outside this target: slice 111's `bootstrap::HttpProviderBackend` adapts
`oran-http::Client` to `provider::ProtocolTransport`, and slice 112 uses that
backend from configured-route `bootstrap::run`; slices 121-124 add SSE transport
and provider streaming decode while preserving that dependency direction.
Provider lifecycle hooks also stay outside this target: slice 126 publishes them
from `oran-agent`, so the provider domain remains hook-free.

`oran-memory` ships the slice-130 session-store wrapper over
`storage::SessionRepository`. It depends downward on `oran-storage` for the
SQLite repository and on `oran-async` for awaitable APIs, and it consumes
`nlohmann_json` only in `src/oran-memory/session.cpp` to keep message JSON out of
`oran-storage` and out of public headers. Slice 131 makes `oran-bootstrap`
depend on `oran-memory` as the composition root for the separate
`<workspace>/.orangutan/sessions.db` pool/repository/store; the agent runner
persistence slice consumes that owner next.

`oran-automation` currently ships deterministic periodic scheduling,
long-term memory retention request planning, the slice-189
`AutomationRepository` persistence boundary for retention job/run state, the
slice-190 `MemoryRetentionService` caller-driven tick owner, the slice 191
optional periodic `memory_decay` hook producer, the slice-192
`AutomationRuntime` caller-owned state handle for explicit open/migrate
ownership, the slice-193 `MemoryRetentionLoop` caller-started run-once step,
the slice-194 retention job lifecycle hook producer, and the slice-195
retention job lease migration/API plus due-run loop lease ownership, the
slice-196 finite caller-owned loop policy, and the slice-198 durable cron job
repository state. It depends
downward on `oran-core` for `Result<T>` / `Time`, `oran-async` for awaitable
repository/service/runtime APIs, `oran-storage` for `Pool` / migrations, and
`oran-memory` for the public `memory::longterm::DecayRequest` and `Backend`
contracts, and `oran-hook` for the shared `memory_decay` and job lifecycle event
payloads/bus when the caller explicitly supplies one. It does not yet depend on `oran-agent`
because process service-loop timers, broader agent/category leases, agent
firing, and notifier routing remain downstream. It is registered with
`test-automation` and `bench-automation`.
Slice 188 makes `oran-bootstrap` and the main `orangutan` binary link it for
config-to-retention job descriptor mapping only; slices 189-198 add
repository/tick/hook-production/runtime-state/loop-step/lifecycle/lease/loop-policy
and cron-state ownership without starting an automation service loop or making
bootstrap open `automation.db`.

`oran-bootstrap` depends on `oran-automation` so configured-route startup can
map `memory.longterm.retention` into an automation-owned
`MemoryRetentionJob` descriptor while keeping automation independent of
`oran-config`. It depends on `oran-skill` so the runner can own the pre-rendered
section-4 catalog outside the stable system preamble. It also depends on
`oran-provider` so process startup can preflight the
configured default provider route before handing prompts to `oran-cli`. It also
depends on `oran-agent` for slice 101's `AgentPromptRunner`, the bootstrap-owned
`cli::PromptRunner` implementation that wraps a caller-supplied provider backend in
`provider::execution::Runtime`, binds the CLI approval sink, and drives
`agent::Loop` with runtime-assembly services, including the process hook bus for
slice-126 provider lifecycle events. It now also depends on `oran-http` for the
explicit `HttpProviderBackend` construction seam that can resolve credentials
and produce a real HTTP-backed provider system for callers.
Configured-route `bootstrap::run` now calls that seam, reads the configured
API-key environment variables at the credential boundary, and hands prompts to
`AgentPromptRunner` through `cli::run_async`; the built-in no-route defaults
still use the deterministic synchronous CLI shell.

`oran-cli` depends on `oran-async`, `oran-hook`, and `oran-provider`: slice 95
adds the terminal `OperatorPromptSink`, which implements the blocking
`permission_ask_rendered` decision surface and reads interactive answers through
asio on the current coroutine executor, and slice 123 adds
`cli::StreamingPromptSink`, which implements `provider::EventSink` for live
terminal streaming. The ordinary CLI mode parser remains synchronous.

`oran-agent` now owns the slice-72 `SessionState` promotion owner plus the
slice-80 sequential direct-dispatch `Loop` turn driver with cancellation-phase
context on provider/tool parent cancellations, the first terminal-success
`trace_turns` writer, slice 82's explicit disabled trace gate, and slice 83's
provider/tool cancellation trace rows, plus slice 84's provider/loop-boundary
error trace rows and slice 85's loop-owned turn-id generation for configured
trace writers. The `oran-storage` dependency is an intentional downward
agent-runtime → platform dependency for
`storage::TraceRepository`; the provider dependency is also downward: the agent
runtime layer drives `provider::System`, while `oran-provider` never calls back
into `oran-agent`. Slice 126 adds a downward dependency on `oran-hook` because
the loop publishes advisory provider request/response/error/fallback metadata
around provider awaits. The `orangutan` binary now links `oran-agent`
transitively through `oran-bootstrap` so tests and adapter owners can construct
`AgentPromptRunner`; the configured-route ordinary prompt path now reaches that
runner through `HttpProviderBackend` plus `cli::run_async`, while built-in
no-route defaults remain on the deterministic `cli::run` shell.

`oran-channel` is a shipped interface-layer foundation target as of slice 226.
It depends only on `oran-core` and `oran-async`, exports the adapter trait plus
manager/envelope types, and has matching `test-channel` / `bench-channel`
buckets. Slice 227 adds the in-process `MockChannel` adapter and the
`ChannelPromptRunner` dispatch seam inside the same target (no new xmake
target), and `oran-bootstrap` now depends on `oran-channel` for the
`make_channel_agent_prompt_runner(...)` bridge, so the `orangutan` binary
links `oran-channel` transitively. Slice 229 adds the first gated platform
adapter target: `oran-channel-qq` (plus `test-channel-qq` /
`bench-channel-qq`) exists only under `xmake f --channel_qq=y` and currently
depends on `oran-core`, `oran-async`, and `oran-http`; it is not linked into
the `orangutan` binary until bootstrap registration lands in a later QQ-port
milestone. `oran-channel-webhook` remains a future target.

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
- `slint` generated headers — only `oran-desktop` needs them.
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

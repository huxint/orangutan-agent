-- xmake.lua — Orangutan v2 build root
--
-- Source of truth for the build is docs/BUILD_SYSTEM.md. Slice 0 deliberately
-- keeps the dependency surface minimal: only Catch2 (tests) and nanobench
-- (benches). Heavier packages (asio, fmt, nlohmann_json, spdlog, sqlite3, ...)
-- land in the slice that introduces a library which actually consumes them,
-- per docs/exec-plans/completed/2026-05-14-mvp-build-skeleton-slice-0.md.

set_project("orangutan-v2")
set_version("2.0.0")
set_languages("c++26")
set_warnings("all", "extra")

add_rules("mode.debug", "mode.release", "mode.releasedbg")
add_rules("plugin.compile_commands.autoupdate", { outputdir = ".", lsp = "clangd" })

set_policy("package.requires_lock", true)
set_policy("build.warning", true)

includes("xmake/options.lua")
includes("xmake/toolchain.lua")
includes("xmake/packages.lua")
includes("xmake/targets.lua")
includes("xmake/tests.lua")
includes("xmake/bench.lua")
includes("xmake/checks.lua")

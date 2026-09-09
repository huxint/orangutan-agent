-- Orangutan build root. docs/BUILD_SYSTEM.md owns the build contract.

set_project("orangutan-v2")
set_version("2.0.0")
set_languages("c++26")
set_warnings("all", "extra")

-- C++26 reflection (P2996) is required by the in-repo `oran-core/enum_names.hpp`
-- helper for enum wire names.
-- GCC 16.1 gates reflection behind an opt-in flag.
add_cxxflags("-freflection", { force = true })

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

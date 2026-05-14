-- xmake/toolchain.lua — GCC 16.1 toolchain wiring.
--
-- The compile-budget contract (docs/rules/compile-budget.md) is measured
-- against GCC 16.1. The toolchain probes both the suffixed `gcc-16`/`g++-16`
-- names (Debian/Ubuntu-style) and the unsuffixed `gcc`/`g++` (this dev
-- machine, where 16.1.1 is the default system compiler).

local function pick_cc()
    return (find_program and (find_program("gcc-16") or find_program("gcc"))) or "gcc"
end

local function pick_cxx()
    return (find_program and (find_program("g++-16") or find_program("g++"))) or "g++"
end

toolchain("oran-gcc")
    set_homepage("GCC 16.1 with Orangutan's flags.")
    set_kind("standalone")
    on_load(function (toolchain)
        local cxx = pick_cxx()
        toolchain:set("toolset", "cc",  pick_cc())
        toolchain:set("toolset", "cxx", cxx)
        toolchain:set("toolset", "ld",  cxx)
        toolchain:set("toolset", "sh",  cxx)
        toolchain:set("toolset", "ar",  "ar")
        toolchain:add("cxxflags",
            "-std=c++26",
            "-fdiagnostics-color=always",
            "-fdiagnostics-show-template-tree",
            "-pipe",
            "-fno-plt",
            "-fno-common",
            "-fmacro-prefix-map=" .. os.projectdir() .. "=."
        )
        if has_config("lto") and is_mode("release") then
            toolchain:add("cxxflags", "-flto=auto")
            toolchain:add("ldflags",  "-flto=auto")
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
            -- See docs/rules/static-analysis.md for the required-warning rationale.
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
        if has_config("sanitizers") and is_mode("debug") then
            toolchain:add("cxxflags", "-fsanitize=address,undefined", "-fno-omit-frame-pointer")
            toolchain:add("ldflags",  "-fsanitize=address,undefined")
        end
    end)
toolchain_end()

set_toolchains("oran-gcc")

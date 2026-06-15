-- xmake/packages.lua — third-party requirements.
--
-- Every package listed here must also appear in docs/rules/libraries.md
-- (docs/rules/docs-in-sync.md — Check 6 in scripts/check-docs-sync.sh).
-- Packages land with the library that introduces them, so the default build
-- only pays for dependencies that current targets actually consume.

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

-- Slint desktop GUI toolkit (gated behind `--desktop`). Consumed from the
-- official prebuilt C++ binary release (headers + libslint_cpp.so +
-- slint-compiler), so no Rust toolchain enters any build path. The download is
-- pinned by sha256; provenance is recorded in docs/SUPPLY_CHAIN_SECURITY.md and
-- the library is documented in docs/rules/libraries.md (Optional table).
package("slint")
    set_kind("library")
    set_homepage("https://slint.dev")
    set_description("Slint declarative GUI toolkit — prebuilt C++ binaries.")
    set_license("GPL-3.0-or-later")

    if is_host("linux") and (os.arch() == "x86_64" or os.arch() == "x64") then
        set_urls("https://github.com/slint-ui/slint/releases/download/v$(version)/Slint-cpp-$(version)-Linux-x86_64.tar.gz")
        add_versions("1.16.1", "125dd01e579c041f80dab0b92522a027b99d7cee47b5c005d6291f4cca639715")
    end

    on_install("linux", function (package)
        -- Headers reference each other relatively (`#include "private/..."`), so
        -- the include root is the dir holding slint.h: flatten include/slint/*.
        os.cp("include/slint/*", package:installdir("include"))
        os.cp("lib/libslint_cpp.so", package:installdir("lib"))
        os.cp("bin/slint-compiler", package:installdir("bin"))
        package:add("links", "slint_cpp")
        package:addenv("PATH", "bin")
        package:addenv("LD_LIBRARY_PATH", "lib")
    end)

    on_test(function (package)
        assert(os.isfile(path.join(package:installdir("bin"), "slint-compiler")),
               "slint-compiler not found in the prebuilt package")
    end)
package_end()

if has_config("desktop") then
    add_requires("slint 1.16.1")
end

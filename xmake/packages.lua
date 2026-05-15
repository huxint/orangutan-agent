-- xmake/packages.lua — third-party requirements.
--
-- Every package listed here must also appear in docs/rules/libraries.md
-- (docs/rules/docs-in-sync.md — Check 6 in scripts/check-docs-sync.sh).
-- Packages land with the library that introduces them, so the default build
-- only pays for dependencies that current targets actually consume.

add_requires("asio 1.36.0")
add_requires("catch2 3.7.1")
add_requires("nanobench 4.3.11")
add_requires("sqlite3 3.51.0+0")

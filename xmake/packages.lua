-- xmake/packages.lua — third-party requirements.
--
-- Every package listed here must also appear in docs/rules/libraries.md
-- (docs/rules/docs-in-sync.md — Check 6 in scripts/check-docs-sync.sh).
-- Slice 0 only depends on what tests and benches need; production-library
-- packages land with the library that introduces them.

add_requires("catch2 3.7.1")
add_requires("nanobench 4.3.11")

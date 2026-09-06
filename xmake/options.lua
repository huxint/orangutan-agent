-- xmake/options.lua — build-time options.
--
-- See docs/BUILD_SYSTEM.md for the documented set. Early slices ship only the
-- options actually consumed by the toolchain or by xmake/targets.lua. New
-- options land alongside the rule or library that depends on them so docs and
-- code stay aligned (docs/rules/docs-in-sync.md).

option("modules")
    set_default(false)
    set_description("Enable C++26 modules (off in slice 0; opt-in for experiments).")
option_end()

option("lto")
    set_default(true)
    set_description("Enable -flto=auto in release builds.")
option_end()

option("hardened")
    set_default(false)
    set_description("Enable hardening flags (_FORTIFY_SOURCE=3, stack/cf protection).")
option_end()

option("sanitizers")
    set_default(false)
    set_description("Enable ASan/UBSan in debug builds.")
option_end()

option("analyze")
    set_default(false)
    set_description("Enable GCC 16.1 -fanalyzer (see docs/rules/static-analysis.md).")
option_end()

option("vector_memory")
    set_default(false)
    set_description("Enable optional sqlite-vec long-term memory vector backend.")
option_end()

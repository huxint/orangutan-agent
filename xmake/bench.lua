-- xmake/bench.lua — nanobench buckets.
--
-- One target per bench/<lib>/ directory. The "A-vs-B" pattern is required by
-- docs/rules/testing-and-bench.md.

local root = os.projectdir()

local function oran_bench(name, deps)
    target("bench-" .. name)
        set_kind("binary")
        set_group("oran-benches")
        set_default(false)
        add_includedirs(path.join(root, "include"), { public = false })
        add_files(path.join(root, "bench", name, "**.cpp"))
        add_deps(table.unpack(deps))
        add_packages("nanobench")
        set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
    target_end()
end

oran_bench("core", { "oran-core" })
oran_bench("async", { "oran-async" })

-- xmake/tests.lua — Catch2 v3 buckets.
--
-- One target per tests/<lib>/ directory. The convention is documented in
-- docs/rules/testing-and-bench.md. Slice 0 ships only tests/core.

local root = os.projectdir()

local function oran_test(name, deps)
    target("test-" .. name)
        set_kind("binary")
        set_group("oran-tests")
        set_default(false)
        add_includedirs(path.join(root, "include"), { public = false })
        add_files(path.join(root, "tests", name, "**.cpp"))
        add_deps(table.unpack(deps))
        add_packages("catch2")
        set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
        add_tests("default", { runargs = { "--reporter=console", "--verbosity=normal" } })
        on_run(function (target)
            os.execv(target:targetfile(), { "--reporter=console", "--verbosity=normal" })
        end)
    target_end()
end

oran_test("core", { "oran-core" })

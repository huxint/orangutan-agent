-- xmake/targets.lua — production targets.
--
-- One xmake target per library declared in docs/ARCHITECTURE.md, plus the
-- public binaries listed there. Slice 0 ships only oran-core + the
-- `orangutan` binary; future slices add libraries one at a time per
-- docs/exec-plans/.

local root = os.projectdir()

local function oran_lib(name, deps, packages)
    target("oran-" .. name)
        set_kind("static")
        set_group("oran-libs")
        add_includedirs(path.join(root, "include"), { public = true })
        add_files(path.join(root, "src", "oran-" .. name, "**.cpp"))
        if deps then
            add_deps(table.unpack(deps))
        end
        if packages then
            add_packages(table.unpack(packages), { public = false })
        end
        set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
    target_end()
end

oran_lib("core", {}, {})

target("orangutan")
    set_kind("binary")
    set_group("oran-bins")
    add_deps("oran-core")
    add_files(path.join(root, "src/main.cpp"))
target_end()

-- xmake/targets.lua — production targets.
--
-- One xmake target per shipped library declared in docs/ARCHITECTURE.md, plus
-- the public binaries listed there. Future slices add libraries one at a time
-- per docs/exec-plans/.

local root = os.projectdir()

local function oran_lib(name, deps, private_packages, public_packages)
    target("oran-" .. name)
        set_kind("static")
        set_group("oran-libs")
        add_includedirs(path.join(root, "include"), { public = true })
        add_files(path.join(root, "src", "oran-" .. name, "**.cpp"))
        if deps then
            add_deps(table.unpack(deps))
        end
        if private_packages then
            add_packages(table.unpack(private_packages), { public = false })
        end
        if public_packages then
            add_packages(table.unpack(public_packages), { public = true })
        end
        set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
    target_end()
end

oran_lib("core", {}, {})
oran_lib("async", { "oran-core" }, {}, { "asio" })

target("orangutan")
    set_kind("binary")
    set_group("oran-bins")
    add_deps("oran-core", "oran-async")
    add_files(path.join(root, "src/main.cpp"))
target_end()

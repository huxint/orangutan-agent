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
            for _, pkg in ipairs(private_packages) do
                add_packages(pkg, { public = false })
            end
        end
        if public_packages then
            for _, pkg in ipairs(public_packages) do
                add_packages(pkg, { public = true })
            end
        end
        set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
    target_end()
end

oran_lib("core", {}, {})
oran_lib("async", { "oran-core" }, {}, { "asio" })
oran_lib("io", { "oran-core", "oran-async" }, {}, { "asio" })
oran_lib("storage", { "oran-core", "oran-async" }, { "sqlite3" })
oran_lib("config", { "oran-core", "oran-storage" }, { "nlohmann_json", "re2" })
oran_lib("permission", { "oran-core", "oran-config", "oran-storage", "oran-async" }, { "re2", "libsodium" })
oran_lib("hook", { "oran-core", "oran-async" }, {})
oran_lib("tool", { "oran-core", "oran-async", "oran-io", "oran-permission", "oran-hook" }, { "nlohmann_json" })
oran_lib("prompt", { "oran-core", "oran-async", "oran-config", "oran-tool" }, {})
oran_lib("provider", { "oran-core", "oran-async", "oran-config", "oran-prompt" }, {})
oran_lib("agent", { "oran-core", "oran-async", "oran-storage", "oran-prompt", "oran-tool", "oran-provider" }, { "nlohmann_json" })
oran_lib("cli", { "oran-core", "oran-async", "oran-hook" }, {})
oran_lib("bootstrap", { "oran-core", "oran-async", "oran-io", "oran-storage", "oran-config", "oran-permission", "oran-hook", "oran-tool", "oran-provider", "oran-agent", "oran-cli" }, {})

target("orangutan")
    set_kind("binary")
    set_group("oran-bins")
    add_deps("oran-core", "oran-async", "oran-io", "oran-storage", "oran-config", "oran-permission", "oran-hook", "oran-tool", "oran-provider", "oran-agent", "oran-cli", "oran-bootstrap")
    add_files(path.join(root, "src/main.cpp"))
    set_rundir(root)
target_end()

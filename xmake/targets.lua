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

local memory_packages = { "nlohmann_json" }
if has_config("vector_memory") then
    table.insert(memory_packages, "sqlite-vec")
end

oran_lib("core", {}, {})
oran_lib("async", { "oran-core" }, {}, { "asio" })
oran_lib("http", { "oran-core", "oran-async" }, { "libcurl" }, { "asio" })
oran_lib("io", { "oran-core", "oran-async" }, {}, { "asio" })
oran_lib("storage", { "oran-core", "oran-async" }, { "sqlite3" })
oran_lib("config", { "oran-core", "oran-storage" }, { "nlohmann_json", "re2" })
oran_lib("permission", { "oran-core", "oran-config", "oran-storage", "oran-async" }, { "re2", "libsodium" })
oran_lib("hook", { "oran-core", "oran-async" }, {})
oran_lib("memory", { "oran-core", "oran-async", "oran-storage" }, memory_packages)
if has_config("vector_memory") then
    target("oran-memory")
        add_defines("ORAN_ENABLE_SQLITE_VEC", { public = true })
    target_end()
end
oran_lib("automation", { "oran-core", "oran-async", "oran-storage", "oran-memory", "oran-hook" }, {})
oran_lib("channel", { "oran-core", "oran-async" }, {})
if has_config("channel_qq") then
    oran_lib("channel-qq", { "oran-core", "oran-async", "oran-http", "oran-channel" }, { "nlohmann_json" }, { "asio" })
end
oran_lib("skill", { "oran-core", "oran-async", "oran-io" }, {})
oran_lib("tool", { "oran-core", "oran-async", "oran-io", "oran-permission", "oran-hook" }, { "nlohmann_json" })
oran_lib("prompt", { "oran-core", "oran-async", "oran-config", "oran-tool" }, {})
oran_lib("provider", { "oran-core", "oran-async", "oran-config", "oran-prompt" }, { "nlohmann_json" })
oran_lib("agent", { "oran-core", "oran-async", "oran-storage", "oran-prompt", "oran-tool", "oran-provider", "oran-hook" }, { "nlohmann_json" })
oran_lib("cli", { "oran-core", "oran-async", "oran-hook", "oran-provider" }, {})

-- oran-desktop: the in-process desktop app (interface-layer peer of oran-cli).
-- The bridge / view-model layer is pure C++ and ALWAYS built (test-desktop runs
-- in every `xmake test`); the Slint UI shell under src/oran-desktop/shell/ plus
-- its `.slint`-generated code compile only with `--desktop=y`, keeping the GUI
-- toolkit's cost off the default build. See docs/DESKTOP.md and
-- docs/exec-plans/active/2026-06-14-oran-desktop-chat-tracer.md.
target("oran-desktop")
    set_kind("static")
    set_group("oran-libs")
    add_includedirs(path.join(root, "include"), { public = true })
    add_files(path.join(root, "src", "oran-desktop", "*.cpp"))  -- top level only (bridge surface)
    add_deps("oran-core", "oran-async", "oran-provider")
    set_pcxxheader(path.join(root, "include/oran/_pch.hpp"))
target_end()
if has_config("desktop") then
    target("oran-desktop")
        add_files(path.join(root, "src", "oran-desktop", "shell", "**.cpp"))
        add_packages("slint", { public = true })
        add_defines("ORAN_ENABLE_DESKTOP", { public = true })
        -- The gated build links Slint, so the resulting artifact is GPL-3.0; declare
        -- it so xmake's license check is satisfied. Slint is triple-licensed
        -- (GPLv3 / royalty-free / commercial) — see docs/rules/libraries.md.
        set_license("GPL-3.0-or-later")
        on_load(function (target)
            local gendir = path.join(target:autogendir(), "slint")
            os.mkdir(gendir)
            target:add("includedirs", gendir)
        end)
        -- Generate the C++ header for each ui/*.slint before any object compiles
        -- so the shell sources can `#include` it. Incremental via depend.on_changed.
        before_build(function (target, opt)
            import("core.project.depend")
            import("utils.progress")
            local pkg = target:pkg("slint")
            assert(pkg, "oran-desktop --desktop build requires the slint package")
            local compiler = path.join(pkg:installdir(), "bin", "slint-compiler")
            local gendir = path.join(target:autogendir(), "slint")
            os.mkdir(gendir)
            for _, slintfile in ipairs(os.files(path.join(os.projectdir(), "src", "oran-desktop", "ui", "*.slint"))) do
                local headerfile = path.join(gendir, path.basename(slintfile) .. ".h")
                depend.on_changed(function ()
                    progress.show((opt and opt.progress) or 0,
                                  "${color.build.object}generating.slint %s", slintfile)
                    os.vrunv(compiler, { slintfile, "-o", headerfile, "-f", "cpp" })
                end, { files = { slintfile }, dependfile = target:dependfile(headerfile) })
            end
        end)
    target_end()
end
oran_lib("bootstrap", { "oran-core", "oran-async", "oran-http", "oran-io", "oran-storage", "oran-config", "oran-permission", "oran-hook", "oran-memory", "oran-automation", "oran-channel", "oran-skill", "oran-tool", "oran-provider", "oran-agent", "oran-cli", "oran-desktop" }, {})
if has_config("channel_qq") then
    target("oran-bootstrap")
        add_deps("oran-channel-qq")
        add_defines("ORAN_ENABLE_CHANNEL_QQ", { public = true })
    target_end()
end

target("orangutan")
    set_kind("binary")
    set_group("oran-bins")
    add_deps("oran-core", "oran-async", "oran-http", "oran-io", "oran-storage", "oran-config", "oran-permission", "oran-hook", "oran-memory", "oran-automation", "oran-skill", "oran-tool", "oran-provider", "oran-agent", "oran-cli", "oran-desktop", "oran-bootstrap")
    add_files(path.join(root, "src/main.cpp"))
    set_rundir(root)
target_end()

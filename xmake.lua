-- ImGuiVRHelper xmake build script
-- SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception

set_xmakever("2.8.2")

set_config("rex_ini", true)

includes("lib/commonlibsse-ng")

set_project("ImGuiVRHelper")
set_license("GPL-3.0")

local version = "1.9.4"
local ver = version:split("%.")
set_version(version)

set_languages("c++23")
set_warnings("allextra")
set_policy("package.requires_lock", true)
add_rules("mode.debug", "mode.releasedbg")
set_defaultmode("releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- Runtime dependencies. Pinned versions resolved into xmake-requires.lock.
add_requires("openvr")
add_requires("imgui", { configs = { dx11 = true, win32 = true } })
add_requires("nlohmann_json") -- still needed for ImportLegacySettings (SCS-shaped blob)
add_requires("toml++") -- helper's own settings file
add_requires("directxtk") -- SimpleMath, used for matrix helpers in VRUtils
add_requires("catch2") -- unit tests (ImGuiVRHelperTests target only)
add_requires("devbench-api 1.5.0") -- optional devbench MCP/REST tools (MIT header; runtime no-op without a devbench host)

-- Optional Tracy profiler instrumentation. Off by default so the shipped build
-- pays nothing — zones compile to no-ops (see src/internal/Profiler.h). Enable
-- to capture VR overlay frame cost with: xmake f --tracy=y && xmake
option("tracy")
set_default(false)
set_description("Enable Tracy profiler instrumentation")
option_end()

-- Local package repo: pins Tracy to the exact upstream commit Community Shaders
-- uses (xmake-repo only ships released tags <= v0.12.2), so the profiler wire
-- protocol matches CS's tooling. See xmake-pkgs/packages/t/tracy.
add_repositories("vrhelper-pkgs xmake-pkgs")

if has_config("tracy") then
    add_requires("tracy 0.13.3")
    add_defines("IMGUI_VR_HELPER_TRACY", "TRACY_ENABLE")
end

target("ImGuiVRHelper")
add_deps("commonlibsse-ng")
add_packages("openvr", "imgui", "nlohmann_json", "toml++", "directxtk", "devbench-api")
if has_config("tracy") then
    add_packages("tracy")
end

set_basename("imgui-vr-helper")

add_shflags("/DEBUG", { force = true })

set_configvar("VERSION_MAJOR", tonumber(ver[1]))
set_configvar("VERSION_MINOR", tonumber(ver[2]))
set_configvar("VERSION_PATCH", tonumber(ver[3]))
set_configvar("VERSION_STRING", version)

-- Numeric version defines available at compile time. The C side
-- stringifies these into IMGUI_VR_HELPER_VERSION_STRING to avoid the
-- shell-quoting headache of passing a quoted string through /D.
add_defines(
    "IMGUI_VR_HELPER_VERSION_MAJOR=" .. ver[1],
    "IMGUI_VR_HELPER_VERSION_MINOR=" .. ver[2],
    "IMGUI_VR_HELPER_VERSION_PATCH=" .. ver[3],
    "IMGUI_VR_HELPER_BUILD_NUMBER=((" .. ver[1] .. "<<16)|(" .. ver[2] .. "<<8)|" .. ver[3] .. ")"
)

add_rules("commonlibsse-ng.plugin", {
    name = "ImGuiVRHelper",
    author = "ImGuiVRHelper contributors",
    description = "OpenVR overlay/input glue for ImGui-based SKSE mods",
})

add_files("src/**.cpp")
add_headerfiles("src/**.h")

-- Build the client-side handshake stub into the helper too, so the
-- helper itself can resolve the same interface for self-test paths.
add_files("api/ImGuiVRHelperAPI.cpp")
add_headerfiles("api/**.h")

add_includedirs("src", "api")
set_pcxxheader("src/pch.h")

-- Auto-deploy on build. The helper looks at, in order:
--   1. SkyrimVRPluginTargets — semicolon-separated list of Data folders
--      (or mod-manager mod folders) for explicit override.
--   2. SkyrimVRPath — community env var. Conventionally points at the
--      SkyrimVR install root (folder containing SkyrimVR.exe), but some
--      setups point it at the Data folder directly. We accept both and
--      probe for SkyrimVR.exe to disambiguate.
--
-- If neither is set, the deploy step is a no-op.
after_build(function(target)
    -- Resolve a SkyrimVRPath value to its Data folder, regardless of
    -- whether the env var pointed at the install root or already at Data.
    local function to_data_folder(p)
        p = p:trim()
        if os.isfile(path.join(p, "SkyrimVR.exe")) then
            return path.join(p, "Data")
        end
        -- Already a Data folder (basename "Data", case-insensitive) —
        -- or a mod-manager mod folder used as a virtual Data root.
        return p
    end

    local function collect_targets()
        local explicit = os.getenv("SkyrimVRPluginTargets")
        if explicit and explicit ~= "" then
            local out = {}
            for _, dir in ipairs(explicit:split(";")) do
                dir = dir:trim()
                if dir ~= "" then
                    table.insert(out, dir)
                end
            end
            return out
        end
        local vr_root = os.getenv("SkyrimVRPath")
        if vr_root and vr_root ~= "" then
            return { to_data_folder(vr_root) }
        end
        return {}
    end

    local targets = collect_targets()
    if #targets == 0 then
        print("No deploy target (set SkyrimVRPath or SkyrimVRPluginTargets)")
        return
    end

    local dll = target:targetfile()
    local pdb = target:symbolfile()
    for _, dir in ipairs(targets) do
        local dest = path.join(dir, "SKSE", "Plugins")
        os.mkdir(dest)
        os.cp(dll, dest)
        if os.isfile(pdb) then
            os.cp(pdb, dest)
        end
        print("Deployed to " .. dest)
    end
end)

-- Headless unit tests for the dependency-free logic (no CommonLibVR / SKSE /
-- Skyrim runtime). Defined last so it doesn't inherit the plugin target's
-- after_build deploy hook. Not built by a plain `xmake` — build & run with:
--   xmake build ImGuiVRHelperTests && xmake run ImGuiVRHelperTests
target("ImGuiVRHelperTests")
set_kind("binary")
set_default(false)
set_group("tests")
add_packages("catch2", "directxtk")
-- <openvr.h> comes from CommonLibVR's vendored copy (the same header the plugin
-- compiles against), not the xrepo openvr package (which nests it as openvr/).
add_includedirs("src", "api", "lib/commonlibsse-ng/extern/openvr/headers")
add_files("tests/**.cpp")
remove_files("tests/d3d11/**.cpp")

includes("tests/d3d11/target.lua")

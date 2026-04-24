-- pull in CommonLibF4 (Dear-Modding-FO4 fork) from lib/commonlibf4 submodule
includes("lib/commonlibf4")

set_project("UnreadNotes")
set_version("1.2.1")
set_license("MIT")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- Local Vortex test mod folder. Best-effort copy — build still succeeds if absent
-- (e.g. on a clean clone). Replaces the post-build step from the old CMakeLists.
local VORTEX_TEST_DIR = "C:/games/Vortex/fallout4/mods/custom-mod-1775213665065/F4SE/Plugins"

-- Dual-API plugin entry. CommonLibF4's default template only exports the
-- modern F4SEPlugin_Version (NG/AE-era F4SE). OG runtime 1.10.163 ships
-- F4SE 0.6.23 which only knows the legacy F4SEPlugin_Query API and rejects
-- DLLs without it ("does not appear to be an F4SE plugin"). The OG and NG/AE
-- F4SEInterface structs are ABI-compatible up through GetPluginInfo, so the
-- shared F4SEPlugin_Load (defined via F4SE_PLUGIN_LOAD in main.cpp) works
-- against both. Placeholders use the rule's ${...} substitution.
local PLUGIN_FILE_DATA = [[
#include <F4SE/F4SE.h>

F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData v{};
    v.PluginVersion({ ${PLUGIN_VERSION_MAJOR}, ${PLUGIN_VERSION_MINOR}, ${PLUGIN_VERSION_PATCH}, 0 });
    v.PluginName("${PLUGIN_NAME}");
    v.AuthorName("${PLUGIN_AUTHOR}");
    v.UsesAddressLibrary(false);
    v.UsesSigScanning(false);
    v.IsLayoutDependent(true);
    v.HasNoStructUse(false);
    v.CompatibleVersions({ F4SE::RUNTIME_1_10_984, F4SE::RUNTIME_LATEST });
    return v;
}();

extern "C" {
    struct LegacyPluginInfo {
        unsigned int infoVersion;
        const char* name;
        unsigned int version;
    };

    __declspec(dllexport) bool F4SEPlugin_Query(const void*, LegacyPluginInfo* a_info) {
        a_info->infoVersion = 1;
        a_info->name = "${PLUGIN_NAME}";
        a_info->version = (${PLUGIN_VERSION_MAJOR} << 16) | (${PLUGIN_VERSION_MINOR} << 8) | ${PLUGIN_VERSION_PATCH};
        return true;
    }
}
]]

target("UnreadNotes")
    add_rules("commonlibf4.plugin", {
        name = "UnreadNotes",
        author = "rotassator",
        description = "FallUI - Unread Notes and Holotapes",
        plugin_file_data = PLUGIN_FILE_DATA
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    after_build(function(target)
        local dll = target:targetfile()
        local dist = path.join(os.projectdir(), "dist", "F4SE", "Plugins", path.filename(dll))
        os.cp(dll, dist)
        cprint("${green}[deploy]${clear} %s", dist)

        local test_dest = path.join(VORTEX_TEST_DIR, path.filename(dll))
        if os.isdir(VORTEX_TEST_DIR) then
            os.cp(dll, test_dest)
            cprint("${green}[deploy]${clear} %s", test_dest)
        else
            cprint("${yellow}[deploy]${clear} skipped Vortex copy (folder not found): %s", VORTEX_TEST_DIR)
        end
    end)

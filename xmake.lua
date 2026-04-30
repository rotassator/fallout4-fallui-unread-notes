-- pull in CommonLibF4 (Dear-Modding-FO4 fork) from lib/commonlibf4 submodule
includes("lib/commonlibf4")

set_project("UnreadNotes")
set_version("1.4.0")
set_license("MIT")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

add_requires("nlohmann_json 3.11.3")  -- Keybinds.json parsing; see commit log for audit

-- CRITICAL: must match commonlibf4's build-time value. Without this our TU
-- sees REL::ID sized for 1 runtime (the header default) while commonlibf4.lib
-- sees it sized for 3 — ODR violation that makes every AE/NG lookup read
-- garbage past the end of our array. OG works because slot 0 is the same in
-- both sizes. Spent hours bisecting before spotting this.
add_defines("COMMONLIB_RUNTIMECOUNT=3")

-- Local test-deploy targets, one per mod manager. Each entry:
--   label   — shown in the build log line
--   root    — manager's own folder; used to gate the copy (skip if absent)
--   plugins — final F4SE/Plugins/ path the DLL goes into; created if missing
-- Build stays successful if any target's root is absent (fresh clone, etc).
local TEST_TARGETS = {
    { label = "Vortex",
      root    = "C:/games/Vortex/fallout4/mods",
      plugins = "C:/games/Vortex/fallout4/mods/custom-mod-1775213665065/F4SE/Plugins" },
    { label = "MO2",
      root    = "C:/games/MO2/mods",
      plugins = "C:/games/MO2/mods/UnreadNotes/F4SE/Plugins" },
}

-- Dual-API plugin entry. CommonLibF4's default template only exports the
-- modern F4SEPlugin_Version (NG/AE-era F4SE). OG runtime 1.10.163 ships
-- F4SE 0.6.23 which only knows the legacy F4SEPlugin_Query API and rejects
-- DLLs without it ("does not appear to be an F4SE plugin"). The OG and NG/AE
-- F4SEInterface structs are ABI-compatible up through GetPluginInfo, so the
-- shared F4SEPlugin_Load (defined via F4SE_PLUGIN_LOAD in main.cpp) works
-- against both. Placeholders use the rule's ${...} substitution.
--
-- Compat declaration strategy:
--   UsesAddressLibrary(true)      — claims AE-era Address Library independence
--                                   (bit for 1.11.137+). F4SE 0.7.5+ on AE sees
--                                   this bit and skips the compatibleVersions
--                                   whitelist check, so a single build covers
--                                   every AE point release (1.11.137 through
--                                   1.11.191) as long as the user has the
--                                   matching version-*.bin installed.
--   CompatibleVersions({1.10.984}) — NG-era F4SE (0.7.2-0.7.4) uses a different
--                                   kCurrentAddressLibrary bit (1.10.980), so
--                                   our AE bit is invisible to it; F4SE falls
--                                   through to this whitelist and matches NG.
--   IsLayoutDependent(true)       — sets the 1.11.137+ structure-layout bit
--                                   (confusingly named — this *claims* our code
--                                   uses that layout, which satisfies AE-era
--                                   F4SE's hasStructureIndependence check).
-- OG is unaffected by any of the above: it uses the legacy Query path and
-- never reads PluginVersionData.
local PLUGIN_FILE_DATA = [[
#include <F4SE/F4SE.h>

F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData v{};
    v.PluginVersion({ ${PLUGIN_VERSION_MAJOR}, ${PLUGIN_VERSION_MINOR}, ${PLUGIN_VERSION_PATCH}, 0 });
    v.PluginName("${PLUGIN_NAME}");
    v.AuthorName("${PLUGIN_AUTHOR}");
    v.UsesAddressLibrary(true);
    v.UsesSigScanning(false);
    v.IsLayoutDependent(true);
    v.HasNoStructUse(false);
    v.CompatibleVersions({ F4SE::RUNTIME_1_10_984 });
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

    add_packages("nlohmann_json")

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    after_build(function(target)
        local dll = target:targetfile()
        local dist = path.join(os.projectdir(), "dist", "F4SE", "Plugins", path.filename(dll))
        os.cp(dll, dist)
        cprint("${green}[deploy]${clear} %s", dist)

        -- Source tree for the MCM config files (defaults INI now, menu JSON
        -- once authored). The dist/ tree is Data/-relative, mirroring mod
        -- manager staging convention.
        local mcmConfigSrc = path.join(os.projectdir(), "dist", "MCM", "Config", "UnreadNotes")

        for _, t in ipairs(TEST_TARGETS) do
            if os.isdir(t.root) then
                -- DLL
                os.mkdir(t.plugins)  -- idempotent; creates F4SE/Plugins on first build
                local dest = path.join(t.plugins, path.filename(dll))
                os.cp(dll, dest)
                cprint("${green}[deploy]${clear} %s", dest)

                -- MCM config tree. t.plugins ends in F4SE/Plugins, so going
                -- up two levels gives the mod root; mod managers stage at
                -- <modRoot>/MCM/Config/UnreadNotes/ (no Data/ prefix — they
                -- add it during deployment to the game).
                local modRoot = path.directory(path.directory(t.plugins))
                local mcmConfigDest = path.join(modRoot, "MCM", "Config", "UnreadNotes")
                os.mkdir(mcmConfigDest)
                os.cp(path.join(mcmConfigSrc, "*"), mcmConfigDest)
                cprint("${green}[deploy]${clear} %s/", mcmConfigDest)
            else
                cprint("${yellow}[deploy]${clear} skipped %s copy (root not found): %s", t.label, t.root)
            end
        end
    end)

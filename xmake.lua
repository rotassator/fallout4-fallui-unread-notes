-- pull in CommonLibF4 (Dear-Modding-FO4 fork) from lib/commonlibf4 submodule
includes("lib/commonlibf4")

set_project("UnreadNotes")
set_version("1.2.1")
set_license("MIT")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

target("UnreadNotes")
    add_rules("commonlibf4.plugin", {
        name = "UnreadNotes",
        author = "rotassator",
        description = "FallUI - Unread Notes and Holotapes"
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

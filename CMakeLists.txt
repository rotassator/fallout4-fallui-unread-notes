# UnreadNotes - F4SE Plugin
#
# CMake build configuration.
#
# CMake is a build system generator - it doesn't compile anything itself,
# it creates project files for your actual compiler (MSVC in our case).
# Think of it as a recipe that says "here are my source files, here's
# what I need to link against, now go make me a DLL."

cmake_minimum_required(VERSION 3.20)
project(UnreadNotes VERSION 1.0.0 LANGUAGES CXX)

# Where the F4SE source code lives (headers + libraries we link against)
set(F4SE_SRC "C:/games/GOG Galaxy/Games/Fallout 4 GOTY/src")

# Paths to the pre-built F4SE libraries.
# We built these from the f4se.sln solution file.
set(F4SE_LIB_DIR "${F4SE_SRC}/f4se/x64/Release")
set(COMMON_LIB_DIR "${F4SE_SRC}/f4se/x64_vc11/Release")

# We're building a DLL (shared library), not an executable.
# F4SE loads this DLL at runtime via LoadLibrary().
# F4SE source files we need to compile into our plugin.
# Unlike a typical library where you just link a .lib, F4SE plugins
# compile the F4SE source files they need directly into their DLL.
# This is because the F4SE code uses RelocAddr to patch into the
# game's address space at runtime - each DLL needs its own copy.
set(F4SE_SOURCES
	# Core Scaleform support
	"${F4SE_SRC}/f4se/f4se/ScaleformValue.cpp"
	"${F4SE_SRC}/f4se/f4se/ScaleformCallbacks.cpp"
	"${F4SE_SRC}/f4se/f4se/GameEvents.cpp"
	# Game engine wrappers (heap allocation, console, etc.)
	"${F4SE_SRC}/f4se/f4se/GameAPI.cpp"
	# Game type implementations (SimpleLock, BSFixedString, etc.)
	"${F4SE_SRC}/f4se/f4se/GameTypes.cpp"
	# Menu system (g_ui, GetMenu, menu table lock, etc.)
	"${F4SE_SRC}/f4se/f4se/GameMenus.cpp"
	# Scaleform memory management
	"${F4SE_SRC}/f4se/f4se/ScaleformAPI.cpp"
	# Game utilities (CRC32, etc.)
	"${F4SE_SRC}/f4se/f4se/GameUtilities.cpp"
	# RelocAddr/RelocPtr base address initialisation
	"${F4SE_SRC}/f4se/f4se_common/Relocation.cpp"
)

add_library(${PROJECT_NAME} SHARED
	src/main.cpp
	${F4SE_SOURCES}
)

# Tell the compiler where to find #include headers.
# We need several include paths because F4SE's code uses includes like
# "f4se/PluginAPI.h", "f4se_common/f4se_version.h", "common/IDebugLog.h".
# Each of these needs a parent directory on the include path.
#
# The "common" library has a quirk: it lives at src/common/common/ but
# its headers are included as "common/IDebugLog.h", so we need both
# src/ (for f4se/ and f4se_common/ prefixed includes) and
# src/common/ (so "common/IDebugLog.h" resolves to src/common/common/IDebugLog.h).
# The F4SE source tree layout:
#   src/f4se/f4se/PluginAPI.h         -> #include "f4se/PluginAPI.h"
#   src/f4se/f4se_common/f4se_version.h -> #include "f4se_common/f4se_version.h"
#   src/common/common/IDebugLog.h     -> #include "common/IDebugLog.h"
target_include_directories(${PROJECT_NAME} PRIVATE
	"${F4SE_SRC}/f4se"
	"${F4SE_SRC}/common"
)

# Use C++17 standard (F4SE uses C++11 minimum, but 17 is fine and gives
# us nicer features for our own code like std::set, structured bindings, etc.)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

# MSVC-specific settings:
#   /MT = static CRT linking (matches how F4SE is built - important!)
#         Without this you'd need the VC++ redistributable installed.
#   WIN32_LEAN_AND_MEAN = skip rarely-used Windows headers (faster compile)
#   RUNTIME = tells F4SE headers we're building for the game runtime,
#             not the Creation Kit editor
#   RUNTIME_VERSION = the packed game version we're targeting (1.10.163)
# Force-include common/IPrefix.h in every source file.
# The entire F4SE codebase relies on this — it's set as a "forced include"
# in the VS projects. IPrefix.h pulls in ITypes.h which defines the base
# types everything uses: UInt8, UInt16, UInt32, UInt64, SInt32, etc.
# Without this, nothing compiles because PluginAPI.h uses UInt32 without
# including the header that defines it. Very C++. Much implicit.
target_compile_options(${PROJECT_NAME} PRIVATE /MT /FI"common/IPrefix.h")
target_compile_definitions(${PROJECT_NAME} PRIVATE
	WIN32_LEAN_AND_MEAN
	RUNTIME
	RUNTIME_VERSION=0x010A0A30
)

# Link against the pre-built F4SE libraries.
# These provide the implementations for IDebugLog, version utilities, etc.
#   common_vc11.lib  - IDebugLog, file I/O, directory iteration
#   f4se_common.lib  - F4SE version info, branch trampolines, utilities
#   f4se_1_10_163.lib - main F4SE library (game types, plugin manager imports)
target_link_libraries(${PROJECT_NAME} PRIVATE
	"${COMMON_LIB_DIR}/common_vc11.lib"
	"${F4SE_LIB_DIR}/f4se_common.lib"
	"${F4SE_LIB_DIR}/f4se_1_10_163.lib"
)

# After building, copy the DLL to the Vortex mod folder so it gets
# deployed to the game's Data/F4SE/Plugins/ directory.
set(VORTEX_MOD_DIR "C:/games/Vortex/fallout4/mods/custom-mod-1775213665065")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	# Copy to dist/ (repo's distributable assets)
	COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist/F4SE/Plugins"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:${PROJECT_NAME}>"
		"${CMAKE_SOURCE_DIR}/dist/F4SE/Plugins/UnreadNotes.dll"
	# Copy to Vortex mod folder for local testing
	COMMAND ${CMAKE_COMMAND} -E make_directory "${VORTEX_MOD_DIR}/F4SE/Plugins"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE:${PROJECT_NAME}>"
		"${VORTEX_MOD_DIR}/F4SE/Plugins/UnreadNotes.dll"
	COMMENT "Copying UnreadNotes.dll to dist/ and Vortex mod folder"
)

# Deploy addon SWF to Vortex: run with `cmake --build build --target deploy-swf`
add_custom_target(deploy-swf
	COMMAND ${CMAKE_COMMAND} -E make_directory "${VORTEX_MOD_DIR}/Interface/UI-Framework/Pipboy"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/addon/UnreadNotesAddon.swf"
		"${VORTEX_MOD_DIR}/Interface/UI-Framework/Pipboy/UnreadNotesAddon.swf"
	COMMENT "Copying UnreadNotesAddon.swf to Vortex mod folder"
)

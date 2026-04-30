// UnreadNotes - F4SE Plugin (CommonLibF4 port)
// Tracks read/unread status for notes and holotapes via the Pip-Boy.
// Pure C++ approach — modifies entryList data for visual indication
// and sorting. AdvanceMovie hook ensures changes display immediately.

#include "pch.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace GFx = Scaleform::GFx;

// Type-aware numeric coercion. CommonLibF4's Value::GetNumber() reads
// _value.number directly (asserting IsNumber); for kUInt/kInt-typed values
// that returns garbage. F4SE's GFxValue::GetNumber switched on type and
// converted — replicate that here for v1.2.1 parity.
static double GFxToNumber(const GFx::Value& v)
{
    if (v.IsNumber())  return v.GetNumber();
    if (v.IsUInt())    return static_cast<double>(v.GetUInt());
    if (v.IsInt())     return static_cast<double>(v.GetInt());
    if (v.IsBoolean()) return v.GetBoolean() ? 1.0 : 0.0;
    return 0.0;
}

static int GFxToInt(const GFx::Value& v) { return static_cast<int>(GFxToNumber(v)); }


// ============================================================================
// Read Tracking Data
// ============================================================================
static std::set<std::uint32_t> g_readNotes;
static std::set<std::uint32_t> g_markedNotes;  // User-flagged items: stay bright, distinct suffix, excluded from auto-read

// Narrow mask for auto-detection via BookMenu/TerminalMenu: notes (0x80)
// and holotapes (0x2000). Misc (0x200) excluded because those items don't
// open BookMenu when used, so we'd never see a "read" event for them.
static constexpr std::uint32_t kFilterMask_ReadableItems = 0x2080;

// Wider mask for manual keypress toggle: also includes misc (0x200) —
// recipes, schematics, contracts. The user can mark them read/unread
// themselves since we have no reliable auto-hook for them.
static constexpr std::uint32_t kFilterMask_MarkableItems = 0x2280;

// Path to the inventory data array in the PipboyMenu Scaleform movie
static const char* kEntryListPath =
    "root.Menu_mc.CurrentPage.List_mc.entryList";


// ============================================================================
// Configuration (loaded via a three-file precedence chain — see GetGameRoot
// and friends below)
// ============================================================================
static int           g_cfgLogLevel = 1;
static float         g_cfgBrightness = 0.5f;
static char          g_cfgSuffix[64] = " (Read)";
static char          g_cfgMarkSuffix[64] = " (*)";
// Previous-load suffix values, snapshotted at the start of each LoadConfig.
// When the user changes a suffix via MCM and the system-menu hot reload fires,
// ModifyEntryListData uses these to strip the stale suffix off entry names
// before appending the new one — without it the new suffix stacks on top of
// the old until Pip-Boy is reopened. Single-snapshot only (handles the common
// single-edit case); a future GetFullName-replacement hook would obviate this.
static char          g_cfgPrevSuffix[64] = "";
static char          g_cfgPrevMarkSuffix[64] = "";
static std::uint32_t g_cfgToggleKey  = 0;   // VK code (per UESP's "DirectX Scan Codes" page); 0 = disabled
static std::uint32_t g_cfgToggleMods = 0;   // bitfield: kModShift | kModCtrl | kModAlt
static std::uint32_t g_cfgMarkKey    = 0;
static std::uint32_t g_cfgMarkMods   = 0;

// Modifier bit values match MCM's Keybinds.json convention (verified by
// binding Shift/Ctrl/Alt + key in MCM and inspecting the resulting modifiers
// integer). Note: this is NOT Windows' MOD_* convention — MCM uses its own.
// MCM accepts Win as a modifier too, but pressing Win triggers OS-level
// shortcuts and the in-game key event is unreliable, so we reject any bit
// outside this mask at read time.
static constexpr std::uint32_t kModShift = 0x1;
static constexpr std::uint32_t kModCtrl  = 0x2;
static constexpr std::uint32_t kModAlt   = 0x4;
static constexpr std::uint32_t kModMask  = kModShift | kModCtrl | kModAlt;
static bool          g_markAllReadPending = false;

// Log macro: only logs if current level >= required level
// 0 = minimal (errors, startup, config), 1 = normal, 2 = debug (perf, per-item)
#define LOG(level, ...) do { if (g_cfgLogLevel >= (level)) REX::INFO(__VA_ARGS__); } while(0)

// Format "N (Ctrl+Shift+KEYNAME)" for a VK code + modifier mask, or
// "0 (disabled)" when disabled. Writes into the provided buffer.
// Used in the startup config log.
static void FormatKeyForLog(std::uint32_t vkCode, std::uint32_t mods,
                            char* out, std::size_t outSize)
{
    if (vkCode == 0)
    {
        std::snprintf(out, outSize, "0 (disabled)");
        return;
    }

    char modBuf[32] = "";
    if (mods & kModCtrl)  strcat_s(modBuf, sizeof(modBuf), "Ctrl+");
    if (mods & kModShift) strcat_s(modBuf, sizeof(modBuf), "Shift+");
    if (mods & kModAlt)   strcat_s(modBuf, sizeof(modBuf), "Alt+");

    char keyName[32] = {};
    UINT scanCode = MapVirtualKeyA(vkCode, MAPVK_VK_TO_VSC);
    if (scanCode != 0)
        GetKeyNameTextA(static_cast<LONG>(scanCode) << 16, keyName, sizeof(keyName));
    std::snprintf(out, outSize, "%u (%s%s)", vkCode, modBuf,
                  keyName[0] ? keyName : "unknown");
}

// Sanitise a suffix value by stripping `<` and `>`, which break FallUI's
// HTML text mode. Writes result into out; logs a warning if anything was
// stripped. Used for both sSuffix and sMarkSuffix.
static void SanitiseSuffix(const char* rawBuf, char* out, std::size_t outSize, const char* fieldName)
{
    std::size_t j = 0;
    for (std::size_t i = 0; rawBuf[i] && j + 1 < outSize; i++)
    {
        if (rawBuf[i] != '<' && rawBuf[i] != '>')
            out[j++] = rawBuf[i];
    }
    out[j] = '\0';

    if (j != std::strlen(rawBuf))
        REX::WARN("UnreadNotes: WARNING — stripped < > from {}: \"{}\" -> \"{}\"",
            fieldName, rawBuf, out);
}

// Normalise a suffix's leading whitespace: trim any leading spaces/tabs the
// user may have typed, then prepend exactly one space so the in-memory value
// is "ready to append" with consistent visual separation. Idempotent — the
// transformation is stable on already-normalised values.
//
// This decouples the on-disk INI value (clean, no leading space, no quotes
// needed) from the displayed text format (always has a separating space).
// Defaults INI ships values like `sSuffix=(Read)` so MCM's textinput shows
// `(Read)` cleanly; the DLL re-adds the space at apply time.
static void NormaliseSuffix(char* buf, std::size_t bufSize)
{
    if (!buf[0]) return;  // empty — leave alone, no decoration

    const char* src = buf;
    while (*src == ' ' || *src == '\t') ++src;
    if (!*src) { buf[0] = '\0'; return; }  // all whitespace == empty

    char temp[64];
    std::snprintf(temp, sizeof(temp), " %s", src);
    strncpy_s(buf, bufSize, temp, _TRUNCATE);
}

// ----------------------------------------------------------------------------
// Configuration paths — three-file precedence chain
// ----------------------------------------------------------------------------
// v1.4.0+ reads settings via a deterministic fallback chain:
//   1. Data/MCM/Settings/UnreadNotes.ini        (MCM-managed user settings)
//   2. Data/F4SE/Plugins/UnreadNotes.ini        (legacy / manual override)
//   3. Data/MCM/Config/UnreadNotes/settings.ini (shipped defaults)
//   4. Hardcoded fallback constants (paranoia)
// The DLL is file-based and indifferent to MCM-the-mod's presence; MCM
// detection only matters for the migration trigger (handled separately).
// On first launch the legacy path (2) is auto-populated by copying (3) to
// it, so non-MCM users get a pre-filled editable file without us shipping
// a separate seed template.

static const std::filesystem::path& GetGameRoot()
{
    static std::filesystem::path s_root;
    if (!s_root.empty()) return s_root;

    char  exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exePath, sizeof(exePath));
    if (len == 0 || len >= sizeof(exePath))
    {
        REX::WARN("UnreadNotes: WARNING — GetModuleFileName failed (len={}), using cwd", len);
        s_root = std::filesystem::current_path();
        return s_root;
    }

    s_root = exePath;
    s_root.remove_filename();
    return s_root;
}

static const char* GetMcmSettingsPath()
{
    static std::string s = (GetGameRoot() / "Data/MCM/Settings/UnreadNotes.ini").make_preferred().string();
    return s.c_str();
}

static const char* GetLegacyIniPath()
{
    static std::string s = (GetGameRoot() / "Data/F4SE/Plugins/UnreadNotes.ini").make_preferred().string();
    return s.c_str();
}

static const char* GetMcmDefaultsPath()
{
    static std::string s = (GetGameRoot() / "Data/MCM/Config/UnreadNotes/settings.ini").make_preferred().string();
    return s.c_str();
}

// MCM's global keybind registry. One file holds bindings for all mods that
// declare hotkey widgets; entries are filtered by modName at read time.
// Written by MCM when the user binds a key in the in-game key picker.
static const char* GetMcmKeybindsPath()
{
    static std::string s = (GetGameRoot() / "Data/MCM/Settings/Keybinds.json").make_preferred().string();
    return s.c_str();
}

// Presence of MCM's own self-config directory is a reliable proxy for "MCM
// is active in this session". Both Vortex and MO2 conditionally deploy
// MCM's files based on whether it's enabled in the active profile.
static bool IsMcmInstalled()
{
    static int cached = -1;
    if (cached < 0)
    {
        std::string p = (GetGameRoot() / "Data/MCM/Config/MCM").make_preferred().string();
        cached = (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
    }
    return cached != 0;
}

// First-run seeding: if the legacy INI doesn't exist, copy the shipped
// defaults to it. After this one-time copy, the legacy file is the user's
// to edit; mod updates don't touch it.
static void EnsureLegacyIniExists()
{
    const char* legacy = GetLegacyIniPath();
    if (GetFileAttributesA(legacy) != INVALID_FILE_ATTRIBUTES)
    {
        REX::INFO("UnreadNotes: legacy config already exists at {}, no seeding needed", legacy);
        return;
    }

    const char* defaults = GetMcmDefaultsPath();
    if (GetFileAttributesA(defaults) == INVALID_FILE_ATTRIBUTES)
    {
        REX::WARN("UnreadNotes: WARNING — defaults INI missing at {}, can't seed legacy", defaults);
        return;
    }

    if (CopyFileA(defaults, legacy, /*bFailIfExists*/ TRUE))
        REX::INFO("UnreadNotes: Seeded legacy config at {} from defaults", legacy);
    else
        REX::WARN("UnreadNotes: WARNING — failed to seed legacy config (err={}): {} -> {}",
                  GetLastError(), defaults, legacy);
}

// ----------------------------------------------------------------------------
// Config readers — walk the precedence chain
// ----------------------------------------------------------------------------
// Each helper returns the path where the value was found (or nullptr if it
// fell through to the supplied default). Callers may use the path to write
// back into the same file (e.g. the auto-reset of debug triggers) without
// guessing where the value originated.

static const char* ReadConfigInt(const char* section, const char* key,
                                 int defaultValue, int& out)
{
    constexpr int kSentinel = INT_MIN;
    const char* paths[] = {
        GetMcmSettingsPath(),
        GetLegacyIniPath(),
        GetMcmDefaultsPath(),
    };
    for (const char* path : paths)
    {
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            continue;
        int v = GetPrivateProfileIntA(section, key, kSentinel, path);
        if (v != kSentinel)
        {
            out = v;
            return path;
        }
    }
    out = defaultValue;
    return nullptr;
}

static const char* ReadConfigString(const char* section, const char* key,
                                    const char* defaultValue,
                                    char* out, std::size_t outSize)
{
    constexpr const char* kSentinel = "\x01__UNREADNOTES_NOT_FOUND__\x01";
    const char* paths[] = {
        GetMcmSettingsPath(),
        GetLegacyIniPath(),
        GetMcmDefaultsPath(),
    };
    for (const char* path : paths)
    {
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            continue;
        GetPrivateProfileStringA(section, key, kSentinel, out,
                                 static_cast<DWORD>(outSize), path);
        if (std::strcmp(out, kSentinel) != 0)
            return path;
    }
    strncpy_s(out, outSize, defaultValue, _TRUNCATE);
    return nullptr;
}

// Read a hotkey from MCM's global Keybinds.json. Returns the path on
// success, nullptr if the file is absent, malformed, or doesn't contain a
// matching entry. Defensively wrapped: nlohmann/json's parse and accessors
// throw on type mismatch and we can't trust the file shape — it's written
// by MCM but a corrupt save / hand-edit / future schema bump could leave it
// in any state. All exceptions are caught and logged; the caller falls
// through to the INI precedence chain on failure.
static const char* ReadMcmHotkey(const char* keybindId,
                                 std::uint32_t& outKey,
                                 std::uint32_t& outMods)
{
    const char* path = GetMcmKeybindsPath();
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return nullptr;

    try
    {
        std::ifstream f(path);
        if (!f.is_open()) return nullptr;

        auto j = nlohmann::json::parse(f);
        if (!j.is_object() || !j.contains("keybinds") || !j["keybinds"].is_array())
            return nullptr;

        for (const auto& kb : j["keybinds"])
        {
            if (!kb.is_object()) continue;

            // Filter by both modName and id. MCM uses globally-unique ids
            // (mod-prefixed by convention), so id alone would suffice — but
            // matching modName too is cheap and lets us diagnose collisions.
            const auto modName = kb.value("modName", std::string{});
            const auto id      = kb.value("id",      std::string{});
            if (modName != "UnreadNotes" || id != keybindId) continue;

            const int keycode   = kb.value("keycode", -1);
            const int modifiers = kb.value("modifiers", 0);

            if (keycode < 0 || keycode > 255)
            {
                REX::WARN("UnreadNotes: WARNING — Keybinds.json {} has out-of-range keycode={}, ignoring",
                          keybindId, keycode);
                return nullptr;
            }

            REX::INFO("UnreadNotes: MCM-Keybinds {} -> keycode={} modifiers={}",
                      keybindId, keycode, modifiers);

            // Reject any modifier bit we don't recognize (Win, Hyper, future
            // additions). Better to ignore the binding with a clear warning
            // than to silently match the wrong combination because we masked
            // off bits the user actually wanted.
            if ((modifiers & ~static_cast<int>(kModMask)) != 0)
            {
                REX::WARN("UnreadNotes: WARNING — {} has unsupported modifier bits "
                          "(modifiers={}, recognized={}); binding ignored. "
                          "Re-bind using only Shift/Ctrl/Alt.",
                          keybindId, modifiers, static_cast<int>(kModMask));
                return nullptr;
            }

            outKey  = static_cast<std::uint32_t>(keycode);
            outMods = static_cast<std::uint32_t>(modifiers) & kModMask;
            return path;
        }

        // File exists and parses, but no entry for our id. User cleared the
        // binding in MCM (or never set it). Log explicitly so the diagnostic
        // story is complete — otherwise a clear-and-restart produces a
        // silent gap in the log.
        REX::INFO("UnreadNotes: MCM-Keybinds {} -> not bound", keybindId);
    }
    catch (const nlohmann::json::exception& e)
    {
        REX::WARN("UnreadNotes: WARNING — failed to parse {}: {}", path, e.what());
    }
    catch (const std::exception& e)
    {
        REX::WARN("UnreadNotes: WARNING — error reading {}: {}", path, e.what());
    }
    return nullptr;
}

// Sentinel for "MCM is installed and the binding is intentionally absent" —
// distinguishes from "no source matched, falling back to defaults." Pointer
// identity is what ConfigSourceLabel checks; the string is just for grep.
static const char kMcmUnboundSentinel[] = "<MCM-Unbound>";

// Short label for a config source path. Used by LoadConfig's source summary
// log line so a single glance at the F4SE log shows where each setting
// resolved from. Path-derived (mirrors `MCM-Settings` style) rather than
// role-named — the F4SE/Plugins file isn't strictly "legacy" since it's the
// canonical user-editable file for non-MCM users.
static const char* ConfigSourceLabel(const char* path)
{
    if (path == kMcmUnboundSentinel) return "MCM-Unbound";
    if (!path) return "Defaults";  // no path matched anywhere — same effect
    if (std::strcmp(path, GetMcmKeybindsPath()) == 0) return "MCM-Keybinds";
    if (std::strcmp(path, GetMcmSettingsPath()) == 0) return "MCM-Settings";
    if (std::strcmp(path, GetLegacyIniPath())  == 0) return "F4SE-Plugins";
    if (std::strcmp(path, GetMcmDefaultsPath()) == 0) return "Defaults";
    return "?";
}


// ----------------------------------------------------------------------------
// First-launch setup — migration (MCM users) or legacy seeding (non-MCM)
// ----------------------------------------------------------------------------
// Branches on MCM presence. For MCM users: check for v1.3.0 → v1.4.0 config
// migration. For non-MCM users: seed the legacy override file with defaults
// so they have a pre-filled editable file. Each branch is idempotent.
//
// Why split: with MCM, the legacy file's existence is an unambiguous signal
// of v1.3.0 user data and triggers migration. Auto-seeding the legacy for
// MCM users would muddy that signal and cause the migration trigger to fire
// against just-seeded defaults on the second launch. With MCM not installed,
// the legacy file is the canonical user-editable override file and seeding
// it with defaults is the friendly thing to do.

static void MigrateLegacyHotkeysToKeybinds();  // defined just below

static void MaybeMigrateLegacyToMcm()
{
    const char* mcmSettings = GetMcmSettingsPath();
    const char* legacy      = GetLegacyIniPath();

    // Already migrated, or MCM has been used to write at least one setting.
    if (GetFileAttributesA(mcmSettings) != INVALID_FILE_ATTRIBUTES)
    {
        REX::INFO("UnreadNotes: migration skipped — {} already exists", mcmSettings);
        return;
    }

    // Nothing to migrate from.
    if (GetFileAttributesA(legacy) == INVALID_FILE_ATTRIBUTES)
    {
        REX::INFO("UnreadNotes: migration skipped — no legacy config to migrate from ({})", legacy);
        return;
    }

    // Make sure Data/MCM/Settings/ exists. Normally it does (MCM creates it),
    // but be defensive — WritePrivateProfileStringA fails silently if not.
    std::string settingsDir = (GetGameRoot() / "Data/MCM/Settings").string();
    CreateDirectoryA(settingsDir.c_str(), nullptr);  // idempotent (returns FALSE if exists)

    // Copy all sections + key=value pairs from legacy to MCM Settings. We
    // strip comments and blank lines via GetPrivateProfileSection — MCM
    // Settings INIs are values-only by convention (see Upscaling.ini for
    // reference). Comments live in the defaults file.
    char sectionsBuf[8192] = {};
    DWORD len = GetPrivateProfileSectionNamesA(sectionsBuf, sizeof(sectionsBuf), legacy);
    if (len == 0 || len >= sizeof(sectionsBuf) - 2)
    {
        REX::WARN("UnreadNotes: WARNING — section enumeration of {} failed or truncated; aborting migration", legacy);
        return;
    }

    int copiedKeys = 0;
    for (const char* section = sectionsBuf; *section; section += std::strlen(section) + 1)
    {
        char sectionData[8192] = {};
        DWORD slen = GetPrivateProfileSectionA(section, sectionData, sizeof(sectionData), legacy);
        if (slen == 0 || slen >= sizeof(sectionData) - 2)
            continue;

        for (const char* entry = sectionData; *entry; entry += std::strlen(entry) + 1)
        {
            const char* eq = std::strchr(entry, '=');
            if (!eq) continue;
            std::string key(entry, eq - entry);
            std::string value(eq + 1);
            std::string destSection = section;

            // Hotkeys belong in Data/MCM/Settings/Keybinds.json now, not in
            // MCM-Settings INI — MigrateLegacyHotkeysToKeybinds (called below)
            // handles them. Skip here so they don't pollute MCM-Settings as
            // dead weight.
            if (key == "iToggleKey" || key == "iMarkKey")
                continue;

            // v1.4.0 normalisation — see CleanupOrphanedKeys for the
            // idempotent post-migration cleanup that handles already-
            // migrated users with the pre-1.4.0 layout.
            if (key == "iLogLevel")
            {
                // Section moved [Display] -> [Debug]: it's a debug knob, not a
                // display knob.
                destSection = "Debug";
            }
            else if (key == "sSuffix" || key == "sMarkSuffix")
            {
                // Normalise: strip wrapping quotes (INI syntax for preserving
                // a leading space) and trim leading whitespace. The DLL
                // re-prepends a single space at apply time via NormaliseSuffix,
                // so on-disk values stay clean and MCM's textinput shows the
                // visible part of the suffix without quote-noise.
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                    value = value.substr(1, value.size() - 2);
                std::size_t start = value.find_first_not_of(" \t");
                if (start == std::string::npos) value.clear();
                else                            value = value.substr(start);
            }

            if (WritePrivateProfileStringA(destSection.c_str(), key.c_str(), value.c_str(), mcmSettings))
                ++copiedKeys;
        }
    }

    REX::INFO("UnreadNotes: migrated {} config values from {} to {}",
              copiedKeys, legacy, mcmSettings);

    // Hotkeys live in Keybinds.json now, not MCM-Settings — handle them
    // separately so the user's v1.3.0 bindings show up in the MCM picker.
    MigrateLegacyHotkeysToKeybinds();

    // Prepend tombstone to the legacy file. Read once, check for our marker,
    // skip if already present so re-migrations don't stack tombstones.
    constexpr const char* kTombstoneMarker = "UnreadNotes \xE2\x80\x94 config migrated to the MCM directory layout";

    std::string original;
    FILE* fr = nullptr;
    fopen_s(&fr, legacy, "rb");
    if (!fr)
    {
        REX::WARN("UnreadNotes: WARNING — couldn't open {} for read during tombstone prepend", legacy);
        return;
    }
    std::fseek(fr, 0, SEEK_END);
    long fileSize = std::ftell(fr);
    std::fseek(fr, 0, SEEK_SET);
    if (fileSize > 0)
    {
        original.resize(static_cast<std::size_t>(fileSize));
        std::fread(original.data(), 1, original.size(), fr);
    }
    std::fclose(fr);

    if (original.find(kTombstoneMarker) != std::string::npos)
        return;

    // Tombstone wording is state-agnostic: describes the new location and
    // shadow precedence without claiming MCM is currently active. Stays
    // accurate even if the user later disables MCM.
    static constexpr const char* kTombstone =
        ";\n"
        "; UnreadNotes \xE2\x80\x94 config migrated to the MCM directory layout (v1.4.0+).\n"
        ";\n"
        "; Defaults:  Data/MCM/Config/UnreadNotes/settings.ini   (shipped, refreshed each release)\n"
        "; Overrides: Data/MCM/Settings/UnreadNotes.ini          (your customisations live here)\n"
        ";\n"
        "; This file's values are kept intact below for downgrade safety, but they\n"
        "; are SHADOWED by the MCM Settings file when both contain the same key.\n"
        "; Edit MCM Settings (via the MCM in-game menu, or directly) to change\n"
        "; settings \xE2\x80\x94 your edits to the values below will not have any effect\n"
        "; while MCM Settings exists.\n"
        ";\n"
        "; (original content below)\n"
        "\n";

    FILE* fw = nullptr;
    fopen_s(&fw, legacy, "wb");
    if (!fw)
    {
        REX::WARN("UnreadNotes: WARNING — couldn't open {} for write during tombstone prepend", legacy);
        return;
    }
    std::fwrite(kTombstone, 1, std::strlen(kTombstone), fw);
    if (!original.empty())
        std::fwrite(original.data(), 1, original.size(), fw);
    std::fclose(fw);

    REX::INFO("UnreadNotes: prepended migration tombstone to {}", legacy);
}

// Carry v1.3.0 hotkeys from F4SE-Plugins INI into MCM's global Keybinds.json.
// Without this, an upgrading user who installs MCM would see "no key bound"
// in the picker even though their iToggleKey/iMarkKey is still active via the
// F4SE-Plugins fallback in loadKey. Re-binding via the picker would then
// silently win over the carried-over INI value.
//
// Idempotent: if the file already contains an entry with our id, skip it.
// Other mods' entries (e.g. CompanionTakeAll's two bindings) are preserved.
// All file I/O and JSON access is wrapped in try/catch — a corrupt or hand-
// edited Keybinds.json must not take the plugin down. On parse failure we
// rebuild from a known-good empty structure rather than overwriting blindly,
// but we WILL clobber unreadable JSON if we already know we have something to
// write. (No shipped mod we know of writes anything we'd want to preserve in
// a state that doesn't parse.)
static void MigrateLegacyHotkeysToKeybinds()
{
    const char* legacy = GetLegacyIniPath();
    if (GetFileAttributesA(legacy) == INVALID_FILE_ATTRIBUTES) return;

    constexpr int kSentinel = INT_MIN;
    auto readKey = [&](const char* iniName) -> int {
        int v = GetPrivateProfileIntA("Input", iniName, kSentinel, legacy);
        if (v == kSentinel || v <= 0 || v > 255) return 0;
        return v;
    };

    const int toggleKey = readKey("iToggleKey");
    const int markKey   = readKey("iMarkKey");

    if (toggleKey == 0 && markKey == 0)
    {
        REX::INFO("UnreadNotes: hotkey migration skipped — no v1.3.0 hotkeys set in {}", legacy);
        return;
    }

    const char* keybindsPath = GetMcmKeybindsPath();
    nlohmann::json j;
    bool hadValidExisting = false;

    try
    {
        if (GetFileAttributesA(keybindsPath) != INVALID_FILE_ATTRIBUTES)
        {
            std::ifstream f(keybindsPath);
            if (f.is_open())
            {
                j = nlohmann::json::parse(f);
                if (j.is_object() && j.contains("keybinds") && j["keybinds"].is_array())
                    hadValidExisting = true;
            }
        }
    }
    catch (const std::exception& e)
    {
        REX::WARN("UnreadNotes: hotkey migration — couldn't parse existing {} ({}); will rewrite",
                  keybindsPath, e.what());
    }

    if (!hadValidExisting)
        j = nlohmann::json{ {"keybinds", nlohmann::json::array()}, {"version", 1} };

    auto& arr = j["keybinds"];

    auto hasEntry = [&](const std::string& id) {
        for (const auto& kb : arr)
            if (kb.is_object() && kb.value("id", std::string{}) == id) return true;
        return false;
    };

    auto addEntry = [&](const char* id, int keycode) -> bool {
        if (keycode <= 0 || keycode > 255) return false;
        if (hasEntry(id)) return false;
        arr.push_back({
            {"id",        id},
            {"modName",   "UnreadNotes"},
            {"keycode",   keycode},
            {"modifiers", 0},
        });
        return true;
    };

    int added = 0;
    if (addEntry("UnreadNotes_toggle", toggleKey)) ++added;
    if (addEntry("UnreadNotes_mark",   markKey))   ++added;

    if (added == 0)
    {
        REX::INFO("UnreadNotes: hotkey migration — no new entries needed (already present in {})", keybindsPath);
        return;
    }

    if (!j.contains("version") || !j["version"].is_number_integer())
        j["version"] = 1;

    try
    {
        std::ofstream out(keybindsPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            REX::WARN("UnreadNotes: hotkey migration — couldn't open {} for write", keybindsPath);
            return;
        }
        out << j.dump();
    }
    catch (const std::exception& e)
    {
        REX::WARN("UnreadNotes: hotkey migration — failed to write {}: {}", keybindsPath, e.what());
        return;
    }

    REX::INFO("UnreadNotes: hotkey migration — wrote {} entr{} to {} (toggle={}, mark={})",
              added, added == 1 ? "y" : "ies", keybindsPath, toggleKey, markKey);
}

// Idempotent post-migration cleanup. Runs every load, fixes up MCM Settings
// entries that arose from v1.4.0 section/key changes when a user migrated
// with a pre-section-move interim build, OR when a v1.3.0 user migrated and
// the migration override only applies prospectively. After cleanup the file
// is in canonical v1.4.0 layout.
static void CleanupOrphanedKeys()
{
    const char* mcmSettings = GetMcmSettingsPath();
    if (GetFileAttributesA(mcmSettings) == INVALID_FILE_ATTRIBUTES)
        return;

    constexpr int kSentinel = INT_MIN;

    // iLogLevel: section moved [Display] -> [Debug] in v1.4.0. If both exist,
    // the [Display] entry is an orphan from a pre-section-move migration.
    if (GetPrivateProfileIntA("Debug",   "iLogLevel", kSentinel, mcmSettings) != kSentinel &&
        GetPrivateProfileIntA("Display", "iLogLevel", kSentinel, mcmSettings) != kSentinel)
    {
        WritePrivateProfileStringA("Display", "iLogLevel", nullptr, mcmSettings);
        REX::INFO("UnreadNotes: cleaned orphan [Display]/iLogLevel from MCM Settings");
    }

    // Hotkey keys: stale if they appear in MCM-Settings — the picker writes
    // to Keybinds.json now, and the migration skips them when copying from
    // F4SE-Plugins. Defensive cleanup catches any that slipped in via manual
    // edit or interim-build state.
    auto dropStaleHotkey = [mcmSettings](const char* keyName) {
        if (GetPrivateProfileIntA("Input", keyName, kSentinel, mcmSettings) == kSentinel)
            return;
        WritePrivateProfileStringA("Input", keyName, nullptr, mcmSettings);
        REX::INFO("UnreadNotes: dropped stale [Input]/{} from MCM Settings", keyName);
    };
    dropStaleHotkey("iToggleKey");
    dropStaleHotkey("iMarkKey");

    // Suffix values: strip leading whitespace from pre-1.4.0 quoted/spaced
    // values so MCM's textinput renders the visible part cleanly. The DLL
    // re-prepends a single space at apply time via NormaliseSuffix, so the
    // displayed suffix in-game is unchanged. Idempotent — once stripped,
    // no leading whitespace, no further work.
    auto cleanupSuffix = [mcmSettings](const char* keyName) {
        char buf[64] = {};
        DWORD len = GetPrivateProfileStringA("Display", keyName, "", buf, sizeof(buf), mcmSettings);
        if (len == 0 || (buf[0] != ' ' && buf[0] != '\t')) return;

        char* p = buf;
        while (*p == ' ' || *p == '\t') ++p;

        WritePrivateProfileStringA("Display", keyName, p, mcmSettings);
        REX::INFO("UnreadNotes: normalised [Display]/{} (stripped leading whitespace)", keyName);
    };
    cleanupSuffix("sSuffix");
    cleanupSuffix("sMarkSuffix");
}

static void RunFirstLaunchSetup()
{
    bool mcm = IsMcmInstalled();
    REX::INFO("UnreadNotes: MCM {}; running {} branch",
              mcm ? "detected" : "not detected",
              mcm ? "migration" : "seeding");

    if (mcm)
        MaybeMigrateLegacyToMcm();
    else
        EnsureLegacyIniExists();

    CleanupOrphanedKeys();
}


static void LoadConfig()
{
    int prevLogLevel = g_cfgLogLevel;
    int logLevel;
    const char* logLevelPath = ReadConfigInt("Debug", "iLogLevel", 1, logLevel);
    if (logLevel < 0) logLevel = 0;
    if (logLevel > 2) logLevel = 2;
    g_cfgLogLevel = logLevel;

    if (prevLogLevel != g_cfgLogLevel && prevLogLevel != 1)  // Don't log on first load (default=1)
        REX::INFO("UnreadNotes: Log level changed to {}", g_cfgLogLevel);

    int rawBrightness;
    const char* brightnessPath = ReadConfigInt("Display", "iReadBrightness", 50, rawBrightness);
    if (rawBrightness < 0 || rawBrightness > 100)
    {
        REX::WARN("UnreadNotes: WARNING — iReadBrightness={} out of range (0-100), clamping",
            rawBrightness);
        if (rawBrightness < 0) rawBrightness = 0;
        if (rawBrightness > 100) rawBrightness = 100;
    }
    g_cfgBrightness = static_cast<float>(rawBrightness) / 100.0f;

    // Snapshot the current suffixes before they get overwritten with new
    // values. Used by ModifyEntryListData / StripKnownSuffixesFromEntry to
    // strip stale suffix text from entry names on hot reload.
    strncpy_s(g_cfgPrevSuffix,     sizeof(g_cfgPrevSuffix),     g_cfgSuffix,     _TRUNCATE);
    strncpy_s(g_cfgPrevMarkSuffix, sizeof(g_cfgPrevMarkSuffix), g_cfgMarkSuffix, _TRUNCATE);

    char suffixBuf[64] = {};
    const char* suffixPath = ReadConfigString("Display", "sSuffix", "(Read)", suffixBuf, sizeof(suffixBuf));
    {
        char sanitised[64] = {};
        SanitiseSuffix(suffixBuf, sanitised, sizeof(sanitised), "sSuffix");
        strncpy_s(g_cfgSuffix, sanitised, sizeof(g_cfgSuffix) - 1);
        NormaliseSuffix(g_cfgSuffix, sizeof(g_cfgSuffix));
    }

    char markSuffixBuf[64] = {};
    const char* markSuffixPath = ReadConfigString("Display", "sMarkSuffix", "(*)", markSuffixBuf, sizeof(markSuffixBuf));
    {
        char sanitised[64] = {};
        SanitiseSuffix(markSuffixBuf, sanitised, sizeof(sanitised), "sMarkSuffix");
        strncpy_s(g_cfgMarkSuffix, sanitised, sizeof(g_cfgMarkSuffix) - 1);
        NormaliseSuffix(g_cfgMarkSuffix, sizeof(g_cfgMarkSuffix));
    }

    // Hotkey precedence:
    //   1. MCM-Keybinds (Data/MCM/Settings/Keybinds.json) — user bound a key
    //      via the in-game key picker
    //   2. F4SE-Plugins INI iName — non-MCM user's scan code (v1.3.0-compatible)
    //   3. Defaults iName=0 — nothing set anywhere, hotkey disabled
    // MCM-Settings is intentionally absent: no shipped version writes hotkeys
    // there (the picker writes to Keybinds.json instead), so probing it would
    // only match stale dev artefacts.
    auto loadKey = [](const char* mcmId, const char* iniName,
                      std::uint32_t& outKey, std::uint32_t& outMods) -> const char* {
        if (const char* path = ReadMcmHotkey(mcmId, outKey, outMods))
            return path;

        // When MCM is installed, it owns the hotkey config: no entry in
        // Keybinds.json means the user explicitly cleared the binding (or
        // never set it). Falling through to F4SE-Plugins INI here would
        // silently resurrect a v1.3.0-era scan code after the user thought
        // they'd disabled the hotkey via the picker. So treat MCM-installed +
        // no-entry as "disabled" and bail out before consulting the INI.
        // Return the MCM-Unbound sentinel so the source log line shows the
        // distinction from "fell through to defaults."
        outMods = 0;
        if (IsMcmInstalled())
        {
            outKey = 0;
            return kMcmUnboundSentinel;
        }

        // Non-MCM path: scan code only, no modifier support.
        constexpr int kSentinel = INT_MIN;
        const char* legacy = GetLegacyIniPath();
        int rawValue = 0;
        const char* path = nullptr;
        if (GetFileAttributesA(legacy) != INVALID_FILE_ATTRIBUTES)
        {
            int v = GetPrivateProfileIntA("Input", iniName, kSentinel, legacy);
            if (v != kSentinel) { rawValue = v; path = legacy; }
        }
        if (!path)
        {
            // Defaults file always present (shipped); fallback to 0.
            rawValue = GetPrivateProfileIntA("Input", iniName, 0, GetMcmDefaultsPath());
        }

        std::uint32_t v = (rawValue >= 0) ? static_cast<std::uint32_t>(rawValue) : 0u;
        if (v > 255)
        {
            REX::WARN("UnreadNotes: WARNING — {}={} out of keyboard range, disabling", iniName, v);
            v = 0;
        }
        outKey = v;
        return path;
    };
    const char* togglePath  = loadKey("UnreadNotes_toggle", "iToggleKey",
                                      g_cfgToggleKey, g_cfgToggleMods);
    const char* markKeyPath = loadKey("UnreadNotes_mark",   "iMarkKey",
                                      g_cfgMarkKey,   g_cfgMarkMods);

    // Reject misconfiguration: identical key + identical modifiers means the
    // mark branch in OnButtonEvent is unreachable. With modifier support, e.g.
    // toggle=\ and mark=Shift+\ is fine — only an exact full-combo collision
    // is a problem.
    if (g_cfgToggleKey != 0 &&
        g_cfgToggleKey  == g_cfgMarkKey &&
        g_cfgToggleMods == g_cfgMarkMods)
    {
        REX::WARN("UnreadNotes: WARNING — toggle and mark are both bound to the same combination "
                  "(key={}, mods={}); disabling mark", g_cfgMarkKey, g_cfgMarkMods);
        g_cfgMarkKey  = 0;
        g_cfgMarkMods = 0;
    }

    // Reject visually ambiguous state: identical non-empty suffixes mean read
    // and marked items look the same.
    if (g_cfgSuffix[0] && g_cfgMarkSuffix[0] && std::strcmp(g_cfgSuffix, g_cfgMarkSuffix) == 0)
    {
        REX::WARN("UnreadNotes: WARNING — sSuffix and sMarkSuffix are identical (\"{}\"); "
            "read and marked items will be visually indistinguishable", g_cfgSuffix);
    }

    char toggleStr[64], markStr[64];
    FormatKeyForLog(g_cfgToggleKey, g_cfgToggleMods, toggleStr, sizeof(toggleStr));
    FormatKeyForLog(g_cfgMarkKey,   g_cfgMarkMods,   markStr,   sizeof(markStr));

    REX::INFO("UnreadNotes: Config — brightness={}% suffix=\"{}\" markSuffix=\"{}\" logLevel={} toggleKey={} markKey={}",
        static_cast<int>(g_cfgBrightness * 100), g_cfgSuffix, g_cfgMarkSuffix, g_cfgLogLevel,
        toggleStr, markStr);

    REX::INFO("UnreadNotes: Sources — iLogLevel={} iReadBrightness={} sSuffix={} sMarkSuffix={} iToggleKey={} iMarkKey={}",
        ConfigSourceLabel(logLevelPath),
        ConfigSourceLabel(brightnessPath),
        ConfigSourceLabel(suffixPath),
        ConfigSourceLabel(markSuffixPath),
        ConfigSourceLabel(togglePath),
        ConfigSourceLabel(markKeyPath));

    // Debug commands — triggered via INI, auto-reset after use. Reset writes
    // back to the path where the trigger value was found (so the trigger
    // state is correctly cleared regardless of which file the user used).
    // Never write to the defaults path — it's the shipped reference and gets
    // overwritten on each mod update.
    int resetAll;
    const char* resetPath = ReadConfigInt("Debug", "bResetAll", 0, resetAll);
    if (resetAll != 0)
    {
        REX::INFO("UnreadNotes: DEBUG — Clearing all {} read notes", g_readNotes.size());
        g_readNotes.clear();
        if (resetPath && std::strcmp(resetPath, GetMcmDefaultsPath()) != 0)
            WritePrivateProfileStringA("Debug", "bResetAll", "0", resetPath);
    }

    int markAll;
    const char* markPath = ReadConfigInt("Debug", "bMarkAllRead", 0, markAll);
    if (markAll != 0)
    {
        REX::INFO("UnreadNotes: DEBUG — bMarkAllRead requested");
        if (markPath && std::strcmp(markPath, GetMcmDefaultsPath()) != 0)
            WritePrivateProfileStringA("Debug", "bMarkAllRead", "0", markPath);
        g_markAllReadPending = true;
    }
}


// ============================================================================
// Serialization (Cosave Persistence)
// ============================================================================
// FourCC tags written into the cosave. Do not change without migrating
// existing save data — byte values are load-bearing for v1.2.1 compat.
static constexpr std::uint32_t make_fourcc(const char (&s)[5])
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) <<  8) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(s[3]));
}

static constexpr std::uint32_t kPluginUID              = make_fourcc("UNrd");
static constexpr std::uint32_t kRecordType_ReadNotes   = make_fourcc("RdNt");
static constexpr std::uint32_t kRecordType_MarkedNotes = make_fourcc("MkNt");
static constexpr std::uint32_t kDataVersion            = 1;

// Pin the byte values against v1.2.1 MSVC multi-char-literal packing — if
// someone ever "fixes" make_fourcc to swap endianness these will fire before
// a broken DLL ships and orphans every user's cosave.
static_assert(kPluginUID              == 0x554E7264u, "UNrd tag must stay 0x554E7264");
static_assert(kRecordType_ReadNotes   == 0x52644E74u, "RdNt tag must stay 0x52644E74");
static_assert(kRecordType_MarkedNotes == 0x4D6B4E74u, "MkNt tag must stay 0x4D6B4E74");

static void WriteFormIDSet(const F4SE::SerializationInterface* intfc,
    std::uint32_t recordType, const std::set<std::uint32_t>& set)
{
    if (!intfc->OpenRecord(recordType, kDataVersion))
        return;
    auto count = static_cast<std::uint32_t>(set.size());
    intfc->WriteRecordData(count);
    for (std::uint32_t formID : set)
        intfc->WriteRecordData(formID);
}

static void ReadFormIDSet(const F4SE::SerializationInterface* intfc,
    std::set<std::uint32_t>& out, const char* label, std::uint32_t recordLength, std::uint32_t version)
{
    std::uint32_t count = 0;
    intfc->ReadRecordData(count);

    // Bounds-check count against the record length supplied by GetNextRecordInfo.
    // A corrupt or hand-edited cosave could otherwise request billions of reads.
    std::uint32_t maxCount = (recordLength > sizeof(std::uint32_t))
        ? (recordLength - sizeof(std::uint32_t)) / sizeof(std::uint32_t)
        : 0;
    if (count > maxCount)
    {
        REX::WARN("UnreadNotes: WARNING — {} record claims {} entries but only {} fit in {} bytes; clamping",
            label, count, maxCount, recordLength);
        count = maxCount;
    }

    std::uint32_t loaded = 0;
    for (std::uint32_t i = 0; i < count; i++)
    {
        std::uint32_t savedFormID = 0;
        intfc->ReadRecordData(savedFormID);
        if (auto resolved = intfc->ResolveFormID(savedFormID))
        {
            out.insert(*resolved);
            loaded++;
        }
        else
        {
            LOG(1, "UnreadNotes: {} FormID {:08X} could not be resolved, skipping",
                label, savedFormID);
        }
    }
    REX::INFO("UnreadNotes: Load — {} of {} {} entries resolved (record v{})",
        loaded, count, label, version);
}

static void Serialization_Revert(const F4SE::SerializationInterface*)
{
    LOG(1, "UnreadNotes: Revert — clearing {} read + {} marked",
        g_readNotes.size(), g_markedNotes.size());
    g_readNotes.clear();
    g_markedNotes.clear();
}

static void Serialization_Save(const F4SE::SerializationInterface* intfc)
{
    LOG(1, "UnreadNotes: Save — writing {} read + {} marked",
        g_readNotes.size(), g_markedNotes.size());
    WriteFormIDSet(intfc, kRecordType_ReadNotes,   g_readNotes);
    WriteFormIDSet(intfc, kRecordType_MarkedNotes, g_markedNotes);
}

static void Serialization_Load(const F4SE::SerializationInterface* intfc)
{
    std::uint32_t type, version, length;

    while (intfc->GetNextRecordInfo(type, version, length))
    {
        if (type == kRecordType_ReadNotes)
        {
            if (version == kDataVersion)
                ReadFormIDSet(intfc, g_readNotes, "read", length, version);
            else
                REX::WARN("UnreadNotes: WARNING — ReadNotes v{} unknown, skipping", version);
        }
        else if (type == kRecordType_MarkedNotes)
        {
            if (version == kDataVersion)
                ReadFormIDSet(intfc, g_markedNotes, "marked", length, version);
            else
                REX::WARN("UnreadNotes: WARNING — MarkedNotes v{} unknown, skipping", version);
        }
        // Unknown record types fall through silently — preserves forward-compat
        // if a future version adds new records that this loader doesn't recognise.
    }

    // Enforce mutual-exclusion invariant. A corrupt or hand-edited cosave could
    // list the same FormID in both sets; marked takes priority in the UI, so
    // prefer that state for consistency.
    std::uint32_t pruned = 0;
    for (std::uint32_t id : g_markedNotes)
        pruned += static_cast<std::uint32_t>(g_readNotes.erase(id));
    if (pruned > 0)
        REX::WARN("UnreadNotes: WARNING — {} FormID(s) in both read and marked sets; "
            "removed from read (marked takes priority)", pruned);
}


// ============================================================================
// Scaleform Functions (accessible from Flash as f4se.plugins.UnreadNotes.*)
// ============================================================================

class ScaleformGetVersion : public GFx::FunctionHandler
{
public:
    void Call(const Params& params) override
    {
        *params.retVal = static_cast<std::uint32_t>(2);
    }
};

class ScaleformIsNoteRead : public GFx::FunctionHandler
{
public:
    void Call(const Params& params) override
    {
        if (params.argCount < 1) { *params.retVal = false; return; }
        auto formID = static_cast<std::uint32_t>(GFxToNumber(params.args[0]));
        *params.retVal = (g_readNotes.count(formID) > 0);
    }
};

class ScaleformGetReadCount : public GFx::FunctionHandler
{
public:
    void Call(const Params& params) override
    {
        *params.retVal = static_cast<std::uint32_t>(g_readNotes.size());
    }
};

template <class T>
static void RegisterScaleformFunction(GFx::Value* parent, GFx::Movie* movie, const char* name)
{
    GFx::Value fn;
    movie->CreateFunction(&fn, new T());
    parent->SetMember(name, fn);
}

static bool ScaleformCallback(GFx::Movie* movie, GFx::Value* value)
{
    RegisterScaleformFunction<ScaleformGetVersion>  (value, movie, "GetVersion");
    RegisterScaleformFunction<ScaleformIsNoteRead>  (value, movie, "IsNoteRead");
    RegisterScaleformFunction<ScaleformGetReadCount>(value, movie, "GetReadCount");
    return true;
}


// ============================================================================
// Pip-Boy Helpers
// ============================================================================
// Direct UI::menuMap.find() crashes on OG runtime — Bethesda's UI hashmap
// layout (tHashSet on OG per F4SE source) doesn't match CommonLibF4's modeled
// BSTHashMap structure on OG, despite working on NG/AE. Workaround: cache
// the PipboyMenu pointer received by the AdvanceMovie vtable hook (first arg)
// while it's actively being advanced, and clear it when the menu closes.

static RE::PipboyMenu* g_lastPipboyMenu = nullptr;

static GFx::Movie* GetPipboyMovie()
{
    auto* menu = g_lastPipboyMenu;
    if (!menu || !menu->uiMovie)
        return nullptr;
    return menu->uiMovie.get();
}

struct ReadableItemInfo
{
    std::uint32_t formID = 0;
    std::uint32_t filterFlag = 0;
    char          tag[32]   = {};   // FallUI _tagStr (e.g. "HolotapeT"); empty if absent
    char          name[128] = {};   // textClean (display name without [Tag] prefix)
};

// Short label for log messages. Prefers FallUI's _tagStr — it distinguishes
// audio holotapes ("HolotapeA") from text ("HolotapeT") and hand-written notes
// ("NoteH") from printed ones. Falls back to filterFlag-derived generic when
// no tag is present (untagged items, or FallUI not installed).
static const char* GetItemTypeLabel(const ReadableItemInfo& info)
{
    if (info.tag[0])                return info.tag;
    if (info.filterFlag & 0x80)     return "note";
    if (info.filterFlag & 0x2000)   return "holotape";
    if (info.filterFlag & 0x200)    return "misc";
    return "item";
}

static bool GetSelectedReadableItem(GFx::Movie* movie, ReadableItemInfo& out,
    std::uint32_t filterMask = kFilterMask_ReadableItems)
{
    GFx::Value selectedEntry;
    if (!movie->GetVariable(&selectedEntry, "root.Menu_mc.CurrentPage.List_mc.selectedEntry"))
        return false;

    if (!selectedEntry.IsObject())
        return false;

    GFx::Value filterFlagVal;
    if (!selectedEntry.HasMember("filterFlag") || !selectedEntry.GetMember("filterFlag", &filterFlagVal))
        return false;

    std::uint32_t filterFlag = filterFlagVal.GetUInt();
    if (!(filterFlag & filterMask))
        return false;

    GFx::Value formIDVal;
    if (!selectedEntry.HasMember("formID") || !selectedEntry.GetMember("formID", &formIDVal))
        return false;

    out.formID     = formIDVal.GetUInt();
    out.filterFlag = filterFlag;

    // FallUI's Parser.as pulls the leading [Token] off the item name into _tagStr
    // and stores the remainder in textClean. Both missing on untagged items or
    // when FallUI isn't installed — harmless, we fall back to filterFlag/formID.
    GFx::Value tagStrVal;
    if (selectedEntry.HasMember("_tagStr") &&
        selectedEntry.GetMember("_tagStr", &tagStrVal) &&
        tagStrVal.IsString())
    {
        strncpy_s(out.tag, tagStrVal.GetString(), sizeof(out.tag) - 1);
    }

    GFx::Value nameVal;
    if (selectedEntry.HasMember("textClean") &&
        selectedEntry.GetMember("textClean", &nameVal) &&
        nameVal.IsString())
    {
        strncpy_s(out.name, nameVal.GetString(), sizeof(out.name) - 1);
    }

    return true;
}


// ============================================================================
// Entry List Data Modification
// ============================================================================
// Appends the correct suffix (read or mark) to entryList entries based on
// their state. Called from the AdvanceMovie hook (inside the render cycle).
// Marked state takes visual priority if somehow both sets contain the item.

// Returns the suffix that an item should display for its current state,
// or an empty string if no suffix applies. Does not allocate.
static const char* ExpectedSuffixForFormID(std::uint32_t formID)
{
    if (g_markedNotes.count(formID) > 0) return g_cfgMarkSuffix;
    if (g_readNotes.count(formID)   > 0) return g_cfgSuffix;
    return "";
}

// Fast path: returns true if any item's text doesn't match its state.
// Runs every frame but only does simple string compares.
static bool QuickCheckEntriesNeedModification(GFx::Value& entryList, std::uint32_t entryCount)
{
    if (!g_cfgSuffix[0] && !g_cfgMarkSuffix[0])
        return false;  // No suffixes configured — nothing to apply

    for (std::uint32_t idx = 0; idx < entryCount; idx++)
    {
        GFx::Value de;
        entryList.GetElement(idx, &de);
        if (!de.IsObject()) continue;

        GFx::Value ffv;
        if (!de.HasMember("filterFlag") || !de.GetMember("filterFlag", &ffv))
            continue;
        if (!(ffv.GetUInt() & kFilterMask_MarkableItems))
            continue;

        GFx::Value fiv;
        if (!de.HasMember("formID") || !de.GetMember("formID", &fiv))
            continue;

        const char* expected = ExpectedSuffixForFormID(fiv.GetUInt());
        if (!expected[0])
            continue;  // Item has no state requiring a suffix

        GFx::Value tv;
        if (!de.HasMember("text") || !de.GetMember("text", &tv) || !tv.IsString())
            continue;

        const char* t = tv.GetString();
        std::size_t tLen = std::strlen(t);
        std::size_t sLen = std::strlen(expected);
        if (tLen < sLen || std::strcmp(t + tLen - sLen, expected) != 0)
            return true;  // Expected suffix missing
    }
    return false;
}

static int ModifyEntryListData(GFx::Movie* /*movie*/, GFx::Value& entryList, std::uint32_t entryCount)
{
    int modified = 0;

    for (std::uint32_t idx = 0; idx < entryCount; idx++)
    {
        GFx::Value dataEntry;
        entryList.GetElement(idx, &dataEntry);
        if (!dataEntry.IsObject()) continue;

        GFx::Value filterFlagVal;
        if (!dataEntry.HasMember("filterFlag") ||
            !dataEntry.GetMember("filterFlag", &filterFlagVal))
            continue;

        std::uint32_t filterFlag = filterFlagVal.GetUInt();
        if (!(filterFlag & kFilterMask_MarkableItems))
            continue;

        GFx::Value formIDVal;
        if (!dataEntry.HasMember("formID") ||
            !dataEntry.GetMember("formID", &formIDVal))
            continue;

        std::uint32_t formID = formIDVal.GetUInt();

        // Debug: mark all readable items as read. Narrower than the outer
        // MarkableItems mask — the misc bit (0x200) covers the entire MISC
        // tab (bobbleheads, quest items, etc.), not just readable schematics,
        // so mass-marking it is wrong. Manual per-item keypress is the only
        // supported way to mark misc items.
        if (g_markAllReadPending && formID != 0 && (filterFlag & kFilterMask_ReadableItems))
            g_readNotes.insert(formID);

        const char* suffix = ExpectedSuffixForFormID(formID);
        if (!suffix[0])
            continue;  // No state — nothing to apply

        GFx::Value textVal;
        if (!dataEntry.HasMember("text") ||
            !dataEntry.GetMember("text", &textVal) ||
            !textVal.IsString())
            continue;

        const char* text = textVal.GetString();
        std::size_t tLen = std::strlen(text);
        std::size_t sLen = std::strlen(suffix);

        // Skip if the correct suffix is already in place
        if (tLen >= sLen && std::strcmp(text + tLen - sLen, suffix) == 0)
            continue;

        // Strip a stale previous-load suffix if present, so the new suffix
        // doesn't stack on top of the old. Only one would be on a given
        // entry at a time (read or marked, never both).
        std::string_view textView{ text, tLen };
        for (std::string_view prev : {
                std::string_view{ g_cfgPrevSuffix },
                std::string_view{ g_cfgPrevMarkSuffix } })
        {
            if (prev.empty() || !textView.ends_with(prev)) continue;
            textView.remove_suffix(prev.size());
            LOG(2, "UnreadNotes: Stripped stale \"{}\" before applying new suffix", prev);
            break;
        }

        char buf[512];
        std::snprintf(buf, sizeof(buf), "%.*s%s",
                      static_cast<int>(textView.size()), textView.data(), suffix);

        GFx::Value newTextVal{ buf };
        dataEntry.SetMember("text", newTextVal);
        modified++;
    }

    if (g_markAllReadPending)
    {
        REX::INFO("UnreadNotes: DEBUG — Marked all readable items as read (total: {})",
            g_readNotes.size());
        g_markAllReadPending = false;
    }

    return modified;
}


// Strip known read or mark suffix from the entry's text if present.
// Called after a state transition so the list refresh shows a clean name
// before ModifyEntryListData re-applies whichever suffix the new state needs.
static void StripKnownSuffixesFromEntry(GFx::Value& entry, const char* itemLabel)
{
    GFx::Value textVal;
    if (!entry.HasMember("text") || !entry.GetMember("text", &textVal) || !textVal.IsString())
        return;

    std::string_view text{ textVal.GetString() };

    for (std::string_view suffix : {
            std::string_view{ g_cfgSuffix },
            std::string_view{ g_cfgMarkSuffix },
            std::string_view{ g_cfgPrevSuffix },
            std::string_view{ g_cfgPrevMarkSuffix } })
    {
        if (suffix.empty() || !text.ends_with(suffix)) continue;

        std::string stripped{ text.substr(0, text.size() - suffix.size()) };
        GFx::Value clean{ stripped.c_str() };
        entry.SetMember("text", clean);
        LOG(2, "UnreadNotes: Stripped \"{}\" from \"{}\"", suffix, itemLabel);
        return;
    }
}


// ============================================================================
// Input Handler — toggle read/unread and mark/unmark
// ============================================================================
// Registered with MenuControls::handlers. Handles two configurable keys:
//   iToggleKey — cycles read/unread. Clears marked state (mutual exclusion).
//   iMarkKey   — cycles marked/unmarked. Clears read state.
// An item is always in exactly one of three states: unread, read, or marked.
// After any transition we strip whatever suffix was present and kick an
// InvalidateData so the display catches up immediately.
class UnreadNotesInputHandler : public RE::BSInputEventUser
{
public:
    // CommonLibF4 BSInputEventUser::ShouldHandleEvent defaults to false.
    // Without this override, OnButtonEvent never fires.
    bool ShouldHandleEvent(const RE::InputEvent*) override { return inputEventHandlingEnabled; }

    void OnButtonEvent(const RE::ButtonEvent* inputEvent) override
    {
        if (!inputEvent)
            return;
        if (inputEvent->device != RE::INPUT_DEVICE::kKeyboard)
            return;

        std::uint32_t key = static_cast<std::uint32_t>(inputEvent->QIDCode());

        // Snapshot current modifier state at the moment the key fires. We
        // require an EXACT match between configured and held mods — bare
        // bindings won't fire on Shift+key, and Shift bindings won't fire on
        // bare key. VK_SHIFT/VK_CONTROL/VK_MENU each cover both left and right
        // variants. We additionally bake in Win-held state via a sentinel bit
        // outside kModMask: any held Win key prevents matching, since our
        // config can't represent Win combinations (we reject those at read).
        constexpr std::uint32_t kWinHeldSentinel = 0x80;  // outside kModMask
        std::uint32_t mods = 0;
        if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= kModShift;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= kModCtrl;
        if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= kModAlt;
        if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)
            mods |= kWinHeldSentinel;

        bool isToggle = (g_cfgToggleKey != 0 && key == g_cfgToggleKey && mods == g_cfgToggleMods);
        bool isMark   = (g_cfgMarkKey   != 0 && key == g_cfgMarkKey   && mods == g_cfgMarkMods);
        if (!isToggle && !isMark)
            return;

        // Initial press only — ignore hold-repeat and release.
        if (!inputEvent->QJustPressed())
            return;

        GFx::Movie* movie = GetPipboyMovie();
        if (!movie)
        {
            LOG(2, "UnreadNotes: Key pressed but Pip-Boy not open");
            return;
        }

        ReadableItemInfo info;
        if (!GetSelectedReadableItem(movie, info, kFilterMask_MarkableItems))
        {
            LOG(2, "UnreadNotes: Key pressed but no markable item selected");
            return;
        }

        const char* direction = "?";
        if (isToggle)
        {
            // Toggle always clears marked, then flips read.
            g_markedNotes.erase(info.formID);
            bool wasRead = (g_readNotes.erase(info.formID) > 0);
            if (!wasRead)
            {
                g_readNotes.insert(info.formID);
                direction = "READ";
            }
            else
            {
                direction = "UNREAD";
            }
        }
        else  // isMark
        {
            // Mark always clears read, then flips marked.
            g_readNotes.erase(info.formID);
            bool wasMarked = (g_markedNotes.erase(info.formID) > 0);
            if (!wasMarked)
            {
                g_markedNotes.insert(info.formID);
                direction = "MARKED";
            }
            else
            {
                direction = "UNMARKED";
            }
        }

        LOG(1, "UnreadNotes: Toggled {} {} \"{}\" (FormID {:08X}) (read={} marked={})",
            direction, GetItemTypeLabel(info), info.name, info.formID,
            g_readNotes.size(), g_markedNotes.size());

        // Strip whichever suffix was present; ModifyEntryListData on the next
        // frame will re-apply the correct one for the new state (if any).
        GFx::Value selectedEntry;
        if (movie->GetVariable(&selectedEntry,
                "root.Menu_mc.CurrentPage.List_mc.selectedEntry") &&
            selectedEntry.IsObject())
        {
            StripKnownSuffixesFromEntry(selectedEntry, info.name);
        }

        // Refresh the list so suffix changes are visible immediately.
        // Alpha dimming updates naturally via AdvanceMovie on the next frame.
        GFx::Value listMc;
        if (movie->GetVariable(&listMc, "root.Menu_mc.CurrentPage.List_mc") &&
            listMc.IsObject())
        {
            listMc.Invoke("InvalidateData");
        }
    }
};

static UnreadNotesInputHandler g_inputHandler;


// ============================================================================
// Menu Event Handler
// ============================================================================
class MenuEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& evn,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
    {
        // MarkAsRead: detect note/holotape activation from Pip-Boy
        if (evn.opening)
        {
            if (evn.menuName == "BookMenu" || evn.menuName == "TerminalMenu")
            {
                if (GFx::Movie* movie = GetPipboyMovie())
                {
                    ReadableItemInfo info;
                    if (GetSelectedReadableItem(movie, info))
                    {
                        if (g_markedNotes.count(info.formID) > 0)
                        {
                            LOG(2, "UnreadNotes: Auto-mark skipped for {} \"{}\" (FormID {:08X}) — item is marked",
                                GetItemTypeLabel(info), info.name, info.formID);
                        }
                        else if (g_readNotes.insert(info.formID).second)
                        {
                            LOG(1, "UnreadNotes: Marked {} \"{}\" (FormID {:08X}) as read via {} (total: {})",
                                GetItemTypeLabel(info), info.name, info.formID, evn.menuName.c_str(),
                                g_readNotes.size());
                        }
                    }
                }
            }
        }

        // Hot-reload config on each Pip-Boy open
        if (evn.opening && evn.menuName == "PipboyMenu")
        {
            LoadConfig();
            LOG(1, "UnreadNotes: PipboyMenu opened");
        }

        // Drop the cached PipboyMenu* when it closes — the pointer becomes
        // dangling once the menu is destroyed.
        if (!evn.opening && evn.menuName == "PipboyMenu")
        {
            g_lastPipboyMenu = nullptr;
            LOG(1, "UnreadNotes: PipboyMenu closed — cached pointer cleared");
        }

        // Hot-reload config when the user returns to Pip-Boy from the system
        // menu (which hosts MCM). Without this, MCM changes made via the
        // system menu while Pip-Boy is open don't apply until Pip-Boy closes
        // and reopens. MCM appears to render within PauseMenu rather than as
        // a separate menu — closing PauseMenu is the canonical "user finished
        // tweaking settings" moment. Gated on Pip-Boy still being open.
        if (!evn.opening &&
            evn.menuName == "PauseMenu" &&
            g_lastPipboyMenu != nullptr)
        {
            LoadConfig();
            LOG(1, "UnreadNotes: PauseMenu closed — config reloaded");
        }

        return RE::BSEventNotifyControl::kContinue;
    }
};

static MenuEventHandler g_menuEventHandler;


// ============================================================================
// AdvanceMovie Hook (PipboyMenu vtable[0] slot 0x04)
// ============================================================================
// Detects holotape playback transitions, applies suffix mods, and dims read
// rows. Hooks via vtable swap (no trampoline / xbyak), so the same compiled
// code resolves correctly across OG/NG/AE via Address Library ID 5497.

using AdvanceMovie_t = void (*)(RE::PipboyMenu*, float, std::uint64_t);
static std::uintptr_t s_OriginalAdvanceMovie = 0;

// Detect a holotape starting playback via the PipboyMenu.DataObj.HolotapePlaying
// flag (public Flash field populated by the game via PopulatePipboyInfoObj).
// Runs every frame the Pipboy is open. Edge-detection only: we mark on the
// false→true transition, NOT on the level. This is essential because an audio
// holotape keeps playing while the player navigates the Pipboy (and even after
// they close it), and we must not keep marking whichever item they hover.
static void DetectHolotapePlayback(GFx::Movie* movie)
{
    static bool s_prevPlaying = false;
    static bool s_initialized = false;

    GFx::Value playingVal;
    bool currentPlaying = false;
    if (movie->GetVariable(&playingVal, "root.Menu_mc.DataObj.HolotapePlaying"))
        currentPlaying = playingVal.GetBoolean();

    // First-ever sample: just record state, don't fire. Handles the case where
    // the player loads a save (or reopens the Pipboy) mid-playback — we'd
    // otherwise see a false→true transition that isn't a real "just played".
    if (!s_initialized)
    {
        s_prevPlaying = currentPlaying;
        s_initialized = true;
        return;
    }

    if (currentPlaying == s_prevPlaying)
        return;

    // Edge detected. Log both directions at level 2 — useful for diagnosing
    // user bug reports ("why didn't my holotape get marked?") without
    // flooding the log like a per-frame heartbeat would.
    ReadableItemInfo info;
    bool haveSelection = GetSelectedReadableItem(movie, info);

    LOG(2, "UnreadNotes: Holotape transition {}→{} — selected: {}{:08X}",
        s_prevPlaying  ? "true"  : "false",
        currentPlaying ? "true"  : "false",
        haveSelection  ? ""         : "(none) ",
        haveSelection  ? info.formID : 0u);

    // Mark only on false→true. Each new holotape produces its own transition
    // cycle: the tape-loading animation briefly sets HolotapePlaying=false
    // before the new audio starts, so seamless play-next-without-stopping
    // still generates a detectable edge.
    if (currentPlaying && !s_prevPlaying && haveSelection)
    {
        if (g_markedNotes.count(info.formID) > 0)
        {
            LOG(2, "UnreadNotes: Auto-mark skipped for {} \"{}\" (FormID {:08X}) — item is marked",
                GetItemTypeLabel(info), info.name, info.formID);
        }
        else if (g_readNotes.insert(info.formID).second)
        {
            LOG(1, "UnreadNotes: Marked {} \"{}\" (FormID {:08X}) as read via HolotapePlaying (total: {})",
                GetItemTypeLabel(info), info.name, info.formID, g_readNotes.size());
        }
    }

    s_prevPlaying = currentPlaying;
}

static void AdvanceMovie_Hook(RE::PipboyMenu* a_menu, float a_timeDelta, std::uint64_t a_time)
{
    // Call original first — lets FallUI do its normal rendering
    reinterpret_cast<AdvanceMovie_t>(s_OriginalAdvanceMovie)(a_menu, a_timeDelta, a_time);

    // Cache the PipboyMenu* so OnButtonEvent / MenuEventHandler can access it
    // without going through UI::menuMap.find (which crashes on OG).
    g_lastPipboyMenu = a_menu;

    if (!a_menu || !a_menu->uiMovie)
        return;

    GFx::Movie* movie = a_menu->uiMovie.get();

    // Holotape playback detection — runs every Pipboy frame regardless of
    // g_readNotes state (we need to catch the very first play of a session).
    DetectHolotapePlayback(movie);

    // Visual work only runs when we actually have something to show
    if (g_readNotes.empty() && !g_markAllReadPending)
        return;

    LARGE_INTEGER perfStart, perfEnd;
    QueryPerformanceCounter(&perfStart);

    GFx::Value entryList;
    if (!movie->GetVariable(&entryList, kEntryListPath) || !entryList.IsArray())
        return;

    std::uint32_t entryCount = entryList.GetArraySize();
    if (entryCount == 0) return;

    // Fast path: only run the full modification walk when something actually
    // needs changing. QuickCheck is O(n) but cheap; ModifyEntryListData does
    // the full work (CreateString, SetMember, InvalidateData) that we want to
    // avoid on frames where FallUI has already picked up our changes.
    bool needsTextMods = g_markAllReadPending ||
        QuickCheckEntriesNeedModification(entryList, entryCount);

    if (needsTextMods)
    {
        int modified = ModifyEntryListData(movie, entryList, entryCount);

        if (modified > 0)
        {
            GFx::Value listMc;
            if (movie->GetVariable(&listMc, "root.Menu_mc.CurrentPage.List_mc") &&
                listMc.IsObject())
            {
                listMc.Invoke("InvalidateData");
            }

            LOG(1, "UnreadNotes: AdvanceMovie — modified {} entries", modified);
        }
    }

    // Apply renderer alpha dimming — dims the entire row including item counts.
    // Runs every frame to handle renderer recycling across subcategory switches.
    // This is lightweight: 14 renderers × (getChildAt + itemIndex lookup + alpha set).
    static const char* kEntryHolderPath =
        "root.Menu_mc.CurrentPage.List_mc.entryHolder_mc";

    GFx::Value entryHolder;
    if (!movie->GetVariable(&entryHolder, kEntryHolderPath) || !entryHolder.IsObject())
        return;

    // numChildren via GetMember off entryHolder. The v1.2.1 dotted-path
    // approach `movie->GetVariable(&v, "...entryHolder_mc.numChildren")`
    // returned 0 on OG via CommonLibF4 even when renderers were present,
    // so read the property directly from the object.
    GFx::Value numRendVal;
    if (!entryHolder.GetMember("numChildren", &numRendVal))
        return;
    int numRenderers = GFxToInt(numRendVal);
    if (numRenderers == 0) return;

    for (int i = 0; i < numRenderers && i < 50; i++)
    {
        GFx::Value indexArg{ static_cast<std::int32_t>(i) };
        GFx::Value renderer;
        if (!entryHolder.Invoke("getChildAt", &renderer, &indexArg, 1))
            continue;
        if (!renderer.IsObject()) continue;

        GFx::Value itemIndexVal;
        if (!renderer.HasMember("itemIndex") ||
            !renderer.GetMember("itemIndex", &itemIndexVal))
            continue;

        int itemIndex = GFxToInt(itemIndexVal);
        if (itemIndex < 0 || itemIndex >= static_cast<int>(entryCount))
            continue;

        GFx::Value dataEntry;
        entryList.GetElement(itemIndex, &dataEntry);
        if (!dataEntry.IsObject()) continue;

        // Determine target alpha based on state.
        //   Marked → full alpha (bright, attention-grabbing).
        //   Read   → configured brightness (dimmed).
        //   Unread → full alpha.
        double targetAlpha = 1.0;

        GFx::Value filterFlagVal;
        if (dataEntry.HasMember("filterFlag") &&
            dataEntry.GetMember("filterFlag", &filterFlagVal))
        {
            std::uint32_t filterFlag = filterFlagVal.GetUInt();
            if (filterFlag & kFilterMask_MarkableItems)
            {
                GFx::Value formIDVal;
                if (dataEntry.HasMember("formID") &&
                    dataEntry.GetMember("formID", &formIDVal))
                {
                    std::uint32_t fid = formIDVal.GetUInt();
                    if (g_markedNotes.count(fid) > 0)
                        targetAlpha = 1.0;
                    else if (g_readNotes.count(fid) > 0)
                        targetAlpha = static_cast<double>(g_cfgBrightness);
                }
            }
        }

        GFx::Value alpha{ targetAlpha };
        renderer.SetMember("alpha", alpha);
    }

    // Performance logging — log every 300th frame (~5s at 60fps) at level 2
    QueryPerformanceCounter(&perfEnd);

    static const LARGE_INTEGER s_perfFreq = []{
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    double microseconds = static_cast<double>(perfEnd.QuadPart - perfStart.QuadPart) * 1000000.0 / static_cast<double>(s_perfFreq.QuadPart);

    static int    s_frameCount = 0;
    static double s_totalUs = 0;
    static double s_maxUs = 0;
    s_frameCount++;
    s_totalUs += microseconds;
    if (microseconds > s_maxUs) s_maxUs = microseconds;

    if (s_frameCount >= 300)
    {
        LOG(2, "UnreadNotes: Perf — avg={:.1f}us max={:.1f}us over {} frames",
            s_totalUs / s_frameCount, s_maxUs, s_frameCount);
        s_frameCount = 0;
        s_totalUs = 0;
        s_maxUs = 0;
    }
}

static void InstallAdvanceMovieHook()
{
    REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE::PipboyMenu[0] };
    s_OriginalAdvanceMovie = vtbl.write_vfunc(0x04, &AdvanceMovie_Hook);
    REX::INFO("UnreadNotes: AdvanceMovie hook installed (vtable slot 0x04 on PipboyMenu)");
}


// ============================================================================
// F4SE Message Handler
// ============================================================================
static void OnF4SEMessage(F4SE::MessagingInterface::Message* msg)
{
    if (msg->type == F4SE::MessagingInterface::kGameDataReady)
    {
        // F4SE packs the bool directly into the data pointer field (not a pointer to a bool)
        bool isReady = msg->data != nullptr;
        if (!isReady)
            return;

        if (auto* ui = RE::UI::GetSingleton())
        {
            ui->RegisterSink<RE::MenuOpenCloseEvent>(&g_menuEventHandler);
            REX::INFO("UnreadNotes: Menu event handler registered");
        }

        if (auto* mc = RE::MenuControls::GetSingleton())
        {
            std::uint32_t before = mc->handlers.size();
            mc->handlers.push_back(&g_inputHandler);
            REX::INFO("UnreadNotes: Input handler registered (toggleKey={}, handlers {}→{})",
                g_cfgToggleKey, before, mc->handlers.size());
        }
        else
        {
            REX::WARN("UnreadNotes: WARNING — MenuControls null, toggle-key feature disabled");
        }
    }
}


// ============================================================================
// Plugin Entry Point
// ============================================================================
F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
    F4SE::Init(a_f4se);

    REX::INFO("UnreadNotes: loading");

    const auto version = REL::Module::GetSingleton()->version();
    const char* tier = REL::Module::IsRuntimeOG() ? "OG" : REL::Module::IsRuntimeNG() ? "NG" : "AE";
    REX::INFO("UnreadNotes: detected runtime {} ({})", version, tier);

    RunFirstLaunchSetup();
    LoadConfig();

    // --- Scaleform ---
    auto* scaleform = F4SE::GetScaleformInterface();
    if (!scaleform)
    {
        REX::CRITICAL("UnreadNotes: couldn't get Scaleform interface");
        return false;
    }
    scaleform->Register("UnreadNotes", ScaleformCallback);

    // --- Serialization ---
    auto* serialization = F4SE::GetSerializationInterface();
    if (!serialization)
    {
        REX::CRITICAL("UnreadNotes: couldn't get Serialization interface");
        return false;
    }
    serialization->SetUniqueID(kPluginUID);
    serialization->SetRevertCallback(Serialization_Revert);
    serialization->SetSaveCallback(Serialization_Save);
    serialization->SetLoadCallback(Serialization_Load);

    // --- Messaging ---
    auto* messaging = F4SE::GetMessagingInterface();
    if (!messaging)
    {
        REX::CRITICAL("UnreadNotes: couldn't get Messaging interface");
        return false;
    }
    messaging->RegisterListener(OnF4SEMessage);

    // --- AdvanceMovie Hook ---
    InstallAdvanceMovieHook();

    REX::INFO("UnreadNotes: loaded successfully");

    return true;
}

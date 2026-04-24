// UnreadNotes - F4SE Plugin (CommonLibF4 port)
// Tracks read/unread status for notes and holotapes via the Pip-Boy.
// Pure C++ approach — modifies entryList data for visual indication
// and sorting. AdvanceMovie hook ensures changes display immediately.

#include "pch.h"

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
// Configuration (loaded from Data/F4SE/Plugins/UnreadNotes.ini)
// ============================================================================
static int           g_cfgLogLevel = 1;
static float         g_cfgBrightness = 0.5f;
static char          g_cfgSuffix[64] = " (Read)";
static char          g_cfgMarkSuffix[64] = " (*)";
static std::uint32_t g_cfgToggleKey = 0;    // VK code (per UESP's "DirectX Scan Codes" page); 0 = disabled
static std::uint32_t g_cfgMarkKey = 0;      // VK code for mark toggle; 0 = disabled
static bool          g_markAllReadPending = false;

// Log macro: only logs if current level >= required level
// 0 = minimal (errors, startup, config), 1 = normal, 2 = debug (perf, per-item)
#define LOG(level, ...) do { if (g_cfgLogLevel >= (level)) REX::INFO(__VA_ARGS__); } while(0)

// Format "N (KEYNAME)" for a VK code, or "0 (disabled)" when disabled.
// Writes into the provided buffer. Used in the startup config log.
static void FormatKeyForLog(std::uint32_t vkCode, char* out, std::size_t outSize)
{
    if (vkCode == 0)
    {
        std::snprintf(out, outSize, "0 (disabled)");
        return;
    }

    char keyName[32] = {};
    UINT scanCode = MapVirtualKeyA(vkCode, MAPVK_VK_TO_VSC);
    if (scanCode != 0)
        GetKeyNameTextA(static_cast<LONG>(scanCode) << 16, keyName, sizeof(keyName));
    std::snprintf(out, outSize, "%u (%s)", vkCode, keyName[0] ? keyName : "unknown");
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

// Build the absolute path to UnreadNotes.ini next to the running executable.
// Using GetModuleFileName avoids mis-targeting when a mod organiser launches
// Fallout 4 with a virtualised or unexpected working directory.
static const char* GetIniPath()
{
    static std::string s_iniPath;
    if (!s_iniPath.empty())
        return s_iniPath.c_str();

    char  exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exePath, sizeof(exePath));
    if (len == 0 || len >= sizeof(exePath))
    {
        REX::WARN("UnreadNotes: WARNING — GetModuleFileName failed (len={}), using relative path", len);
        s_iniPath = R"(Data\F4SE\Plugins\UnreadNotes.ini)";
        return s_iniPath.c_str();
    }

    std::filesystem::path p{ exePath };
    p.remove_filename();
    p /= "Data/F4SE/Plugins/UnreadNotes.ini";
    s_iniPath = p.string();
    return s_iniPath.c_str();
}

static void CreateDefaultConfig(const char* path)
{
    FILE* f = nullptr;
    fopen_s(&f, path, "w");
    if (!f) return;

    std::fprintf(f,
        ";\n"
        "; UnreadNotes - Configuration\n"
        ";\n"
        "; Controls how read notes and holotapes appear in the Pip-Boy inventory.\n"
        ";\n"
        "\n"
        "[Display]\n"
        "\n"
        "; Brightness of read items (0-100).\n"
        "; 100 = full brightness (no change), 50 = half brightness, 0 = invisible.\n"
        "; Default: 50\n"
        "iReadBrightness=50\n"
        "\n"
        "; Text appended to read item names. Use quotes for values with spaces.\n"
        "; Set to \"\" for no suffix. Stick to ASCII characters only — the Pip-Boy\n"
        "; font doesn't support unicode.\n"
        "; Default: \" (Read)\"\n"
        "sSuffix=\" (Read)\"\n"
        "\n"
        "; Text appended to MARKED item names (items flagged with the mark key —\n"
        "; stay bright and are excluded from auto-mark-as-read).\n"
        "; Same ASCII-only rules as sSuffix.\n"
        "; Default: \" (*)\"\n"
        "sMarkSuffix=\" (*)\"\n"
        "\n"
        "; Logging level. 0 = minimal, 1 = normal, 2 = debug (includes perf stats).\n"
        "; Default: 1\n"
        "iLogLevel=1\n"
        "\n"
        "[Input]\n"
        "\n"
        "; Toggle read/unread on the selected Pip-Boy item with a keypress.\n"
        "; Value is the decimal code from the Fallout CK wiki's scan-code table.\n"
        "; Commented out by default so no key is claimed until you opt in.\n"
        "; Reference: https://falloutck.uesp.net/wiki/DirectX_Scan_Codes\n"
        "; Suggested unused keys: 189 (\"-\"), 187 (\"=\"), 220 (\"\\\")\n"
        ";iToggleKey=189\n"
        "\n"
        "; Mark/unmark the selected item. Marked items stay bright, get sMarkSuffix,\n"
        "; and are skipped by auto-mark-as-read. Useful for config holotapes and\n"
        "; notes whose FormIDs are reused across different in-game contexts.\n"
        "; Same reference as iToggleKey; pick a different unused key.\n"
        ";iMarkKey=187\n"
        "\n"
        "[Debug]\n"
        "; Set to 1 and open Pip-Boy to trigger. Auto-resets to 0 after use.\n"
        "bResetAll=0\n"
        "bMarkAllRead=0\n"
    );
    std::fclose(f);
    REX::INFO("UnreadNotes: Created default config at {}", path);
}

static void LoadConfig()
{
    const char* iniPath = GetIniPath();

    DWORD attrs = GetFileAttributesA(iniPath);
    if (attrs == INVALID_FILE_ATTRIBUTES)
        CreateDefaultConfig(iniPath);

    int prevLogLevel = g_cfgLogLevel;
    g_cfgLogLevel = static_cast<int>(GetPrivateProfileIntA("Display", "iLogLevel", 1, iniPath));
    if (g_cfgLogLevel < 0) g_cfgLogLevel = 0;
    if (g_cfgLogLevel > 2) g_cfgLogLevel = 2;

    if (prevLogLevel != g_cfgLogLevel && prevLogLevel != 1)  // Don't log on first load (default=1)
        REX::INFO("UnreadNotes: Log level changed to {}", g_cfgLogLevel);

    int rawBrightness = static_cast<int>(GetPrivateProfileIntA("Display", "iReadBrightness", 50, iniPath));
    if (rawBrightness < 0 || rawBrightness > 100)
    {
        REX::WARN("UnreadNotes: WARNING — iReadBrightness={} out of range (0-100), clamping",
            rawBrightness);
        if (rawBrightness < 0) rawBrightness = 0;
        if (rawBrightness > 100) rawBrightness = 100;
    }
    g_cfgBrightness = static_cast<float>(rawBrightness) / 100.0f;

    char suffixBuf[64] = {};
    GetPrivateProfileStringA("Display", "sSuffix", " (Read)", suffixBuf, sizeof(suffixBuf), iniPath);
    {
        char sanitised[64] = {};
        SanitiseSuffix(suffixBuf, sanitised, sizeof(sanitised), "sSuffix");
        strncpy_s(g_cfgSuffix, sanitised, sizeof(g_cfgSuffix) - 1);
    }

    char markSuffixBuf[64] = {};
    GetPrivateProfileStringA("Display", "sMarkSuffix", " (*)", markSuffixBuf, sizeof(markSuffixBuf), iniPath);
    {
        char sanitised[64] = {};
        SanitiseSuffix(markSuffixBuf, sanitised, sizeof(sanitised), "sMarkSuffix");
        strncpy_s(g_cfgMarkSuffix, sanitised, sizeof(g_cfgMarkSuffix) - 1);
    }

    auto loadKey = [iniPath](const char* name, std::uint32_t& out) {
        std::uint32_t v = static_cast<std::uint32_t>(GetPrivateProfileIntA("Input", name, 0, iniPath));
        if (v > 255)
        {
            REX::WARN("UnreadNotes: WARNING — {}={} out of keyboard range, disabling", name, v);
            v = 0;
        }
        out = v;
    };
    loadKey("iToggleKey", g_cfgToggleKey);
    loadKey("iMarkKey",   g_cfgMarkKey);

    // Reject misconfiguration: both keys set to the same value means the mark
    // branch in OnButtonEvent is unreachable.
    if (g_cfgToggleKey != 0 && g_cfgToggleKey == g_cfgMarkKey)
    {
        REX::WARN("UnreadNotes: WARNING — iToggleKey and iMarkKey are both {}; disabling iMarkKey",
            g_cfgMarkKey);
        g_cfgMarkKey = 0;
    }

    // Reject visually ambiguous state: identical non-empty suffixes mean read
    // and marked items look the same.
    if (g_cfgSuffix[0] && g_cfgMarkSuffix[0] && std::strcmp(g_cfgSuffix, g_cfgMarkSuffix) == 0)
    {
        REX::WARN("UnreadNotes: WARNING — sSuffix and sMarkSuffix are identical (\"{}\"); "
            "read and marked items will be visually indistinguishable", g_cfgSuffix);
    }

    char toggleStr[48], markStr[48];
    FormatKeyForLog(g_cfgToggleKey, toggleStr, sizeof(toggleStr));
    FormatKeyForLog(g_cfgMarkKey,   markStr,   sizeof(markStr));

    REX::INFO("UnreadNotes: Config — brightness={}% suffix=\"{}\" markSuffix=\"{}\" logLevel={} toggleKey={} markKey={}",
        static_cast<int>(g_cfgBrightness * 100), g_cfgSuffix, g_cfgMarkSuffix, g_cfgLogLevel,
        toggleStr, markStr);

    // Debug commands — triggered via INI, auto-reset after use
    if (GetPrivateProfileIntA("Debug", "bResetAll", 0, iniPath) != 0)
    {
        REX::INFO("UnreadNotes: DEBUG — Clearing all {} read notes", g_readNotes.size());
        g_readNotes.clear();
        WritePrivateProfileStringA("Debug", "bResetAll", "0", iniPath);
    }

    if (GetPrivateProfileIntA("Debug", "bMarkAllRead", 0, iniPath) != 0)
    {
        REX::INFO("UnreadNotes: DEBUG — bMarkAllRead requested");
        WritePrivateProfileStringA("Debug", "bMarkAllRead", "0", iniPath);
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

        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s%s", text, suffix);

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

    for (std::string_view suffix : { std::string_view{ g_cfgSuffix }, std::string_view{ g_cfgMarkSuffix } })
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
        bool isToggle = (g_cfgToggleKey != 0 && key == g_cfgToggleKey);
        bool isMark   = (g_cfgMarkKey   != 0 && key == g_cfgMarkKey);
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

// UnreadNotes - F4SE Plugin
// Tracks read/unread status for notes and holotapes via the Pip-Boy.
// Pure C++ approach — modifies entryList data for visual indication
// and sorting. AdvanceMovie hook ensures changes display immediately.

#include "f4se_common/f4se_version.h"
#include "f4se_common/BranchTrampoline.h"
#include "f4se_common/Relocation.h"
#include "f4se/PluginAPI.h"
#include "common/IDebugLog.h"
#include "f4se/ScaleformCallbacks.h"
#include "f4se/GameMenus.h"
#include "f4se/GameInput.h"
#include "xbyak/xbyak.h"

#include <shlobj.h>
#include <set>

static PluginHandle g_pluginHandle = kPluginHandle_Invalid;
static F4SEMessagingInterface* g_messaging = nullptr;


// ============================================================================
// Read Tracking Data
// ============================================================================
static std::set<UInt32> g_readNotes;
static std::set<UInt32> g_markedNotes;  // User-flagged items: stay bright, distinct suffix, excluded from auto-read

// Narrow mask for auto-detection via BookMenu/TerminalMenu: notes (0x80)
// and holotapes (0x2000). Misc (0x200) excluded because those items don't
// open BookMenu when used, so we'd never see a "read" event for them.
static constexpr UInt32 kFilterMask_ReadableItems = 0x2080;

// Wider mask for manual keypress toggle: also includes misc (0x200) —
// recipes, schematics, contracts. The user can mark them read/unread
// themselves since we have no reliable auto-hook for them.
static constexpr UInt32 kFilterMask_MarkableItems = 0x2280;

// Path to the inventory data array in the PipboyMenu Scaleform movie
static const char* kEntryListPath =
	"root.Menu_mc.CurrentPage.List_mc.entryList";


// ============================================================================
// Configuration (loaded from Data/F4SE/Plugins/UnreadNotes.ini)
// ============================================================================
static int    g_cfgLogLevel = 1;
static float  g_cfgBrightness = 0.5f;
static char   g_cfgSuffix[64] = " (Read)";
static char   g_cfgMarkSuffix[64] = " (*)";
static UInt32 g_cfgToggleKey = 0;    // VK code (per UESP's "DirectX Scan Codes" page); 0 = disabled
static UInt32 g_cfgMarkKey = 0;      // VK code for mark toggle; 0 = disabled
static bool   g_markAllReadPending = false;

// Log macro: only logs if current level >= required level
// 0 = minimal (errors, startup, config), 1 = normal, 2 = debug (perf, per-item)
#define LOG(level, ...) do { if (g_cfgLogLevel >= level) _MESSAGE(__VA_ARGS__); } while(0)

// Format "N (KEYNAME)" for a VK code, or "0 (disabled)" when disabled.
// Writes into the provided buffer. Used in the startup config log.
static void FormatKeyForLog(UInt32 vkCode, char* out, size_t outSize)
{
	if (vkCode == 0)
	{
		snprintf(out, outSize, "0 (disabled)");
		return;
	}

	char keyName[32] = {};
	UINT scanCode = MapVirtualKeyA(vkCode, MAPVK_VK_TO_VSC);
	if (scanCode != 0)
		GetKeyNameTextA(static_cast<LONG>(scanCode) << 16, keyName, sizeof(keyName));
	snprintf(out, outSize, "%u (%s)", vkCode, keyName[0] ? keyName : "unknown");
}

// Sanitise a suffix value by stripping `<` and `>`, which break FallUI's
// HTML text mode. Writes result into out; logs a warning if anything was
// stripped. Used for both sSuffix and sMarkSuffix.
static void SanitiseSuffix(const char* rawBuf, char* out, size_t outSize, const char* fieldName)
{
	size_t j = 0;
	for (size_t i = 0; rawBuf[i] && j + 1 < outSize; i++)
	{
		if (rawBuf[i] != '<' && rawBuf[i] != '>')
			out[j++] = rawBuf[i];
	}
	out[j] = '\0';

	if (j != strlen(rawBuf))
		_MESSAGE("UnreadNotes: WARNING — stripped < > from %s: \"%s\" -> \"%s\"",
			fieldName, rawBuf, out);
}

// Build the absolute path to UnreadNotes.ini next to the running executable.
// Using GetModuleFileName avoids mis-targeting when a mod organiser launches
// Fallout 4 with a virtualised or unexpected working directory.
static const char* GetIniPath()
{
	static char s_iniPath[MAX_PATH] = {};
	if (s_iniPath[0] != '\0')
		return s_iniPath;

	char exePath[MAX_PATH];
	DWORD len = GetModuleFileNameA(nullptr, exePath, sizeof(exePath));
	if (len == 0 || len >= sizeof(exePath))
	{
		_MESSAGE("UnreadNotes: WARNING — GetModuleFileName failed (len=%u), using relative path",
			len);
		strcpy_s(s_iniPath, R"(Data\F4SE\Plugins\UnreadNotes.ini)");
		return s_iniPath;
	}

	for (DWORD i = len; i > 0; i--)
	{
		if (exePath[i - 1] == '\\' || exePath[i - 1] == '/')
		{
			exePath[i - 1] = '\0';
			break;
		}
	}

	snprintf(s_iniPath, sizeof(s_iniPath),
		R"(%s\Data\F4SE\Plugins\UnreadNotes.ini)", exePath);
	return s_iniPath;
}

static void CreateDefaultConfig(const char* path)
{
	FILE* f = nullptr;
	fopen_s(&f, path, "w");
	if (!f) return;

	fprintf(f,
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
	fclose(f);
	LOG(0, "UnreadNotes: Created default config at %s", path);
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
		_MESSAGE("UnreadNotes: Log level changed to %d", g_cfgLogLevel);

	int rawBrightness = static_cast<int>(GetPrivateProfileIntA("Display", "iReadBrightness", 50, iniPath));
	if (rawBrightness < 0 || rawBrightness > 100)
	{
		_MESSAGE("UnreadNotes: WARNING — iReadBrightness=%d out of range (0-100), clamping",
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

	auto loadKey = [iniPath](const char* name, UInt32& out) {
		UInt32 v = static_cast<UInt32>(GetPrivateProfileIntA("Input", name, 0, iniPath));
		if (v > 255)
		{
			_MESSAGE("UnreadNotes: WARNING — %s=%u out of keyboard range, disabling", name, v);
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
		_MESSAGE("UnreadNotes: WARNING — iToggleKey and iMarkKey are both %u; disabling iMarkKey",
			g_cfgMarkKey);
		g_cfgMarkKey = 0;
	}

	// Reject visually ambiguous state: identical non-empty suffixes mean read
	// and marked items look the same.
	if (g_cfgSuffix[0] && g_cfgMarkSuffix[0] && strcmp(g_cfgSuffix, g_cfgMarkSuffix) == 0)
	{
		_MESSAGE("UnreadNotes: WARNING — sSuffix and sMarkSuffix are identical (\"%s\"); "
			"read and marked items will be visually indistinguishable", g_cfgSuffix);
	}

	char toggleStr[48], markStr[48];
	FormatKeyForLog(g_cfgToggleKey, toggleStr, sizeof(toggleStr));
	FormatKeyForLog(g_cfgMarkKey,   markStr,   sizeof(markStr));

	LOG(0, "UnreadNotes: Config — brightness=%d%% suffix=\"%s\" markSuffix=\"%s\" logLevel=%d toggleKey=%s markKey=%s",
		static_cast<int>(g_cfgBrightness * 100), g_cfgSuffix, g_cfgMarkSuffix, g_cfgLogLevel,
		toggleStr, markStr);

	// Debug commands — triggered via INI, auto-reset after use
	if (GetPrivateProfileIntA("Debug", "bResetAll", 0, iniPath) != 0)
	{
		_MESSAGE("UnreadNotes: DEBUG — Clearing all %u read notes", static_cast<UInt32>(g_readNotes.size()));
		g_readNotes.clear();
		WritePrivateProfileStringA("Debug", "bResetAll", "0", iniPath);
	}

	if (GetPrivateProfileIntA("Debug", "bMarkAllRead", 0, iniPath) != 0)
	{
		_MESSAGE("UnreadNotes: DEBUG — bMarkAllRead requested");
		WritePrivateProfileStringA("Debug", "bMarkAllRead", "0", iniPath);
		g_markAllReadPending = true;
	}
}


// ============================================================================
// Serialization (Cosave Persistence)
// ============================================================================
// FourCC tags written into the cosave. Hex values match MSVC's packing of
// 'UNrd', 'RdNt', 'MkNt' — do not change without migrating existing save data.
static constexpr UInt32 kPluginUID               = 0x554E7264;  // 'UNrd'
static constexpr UInt32 kRecordType_ReadNotes    = 0x52644E74;  // 'RdNt'
static constexpr UInt32 kRecordType_MarkedNotes  = 0x4D6B4E74;  // 'MkNt'
static constexpr UInt32 kDataVersion             = 1;

static void WriteFormIDSet(const F4SESerializationInterface* intfc,
	UInt32 recordType, const std::set<UInt32>& set)
{
	if (!intfc->OpenRecord(recordType, kDataVersion))
		return;
	auto count = static_cast<UInt32>(set.size());
	intfc->WriteRecordData(&count, sizeof(count));
	for (UInt32 formID : set)
		intfc->WriteRecordData(&formID, sizeof(formID));
}

static void ReadFormIDSet(const F4SESerializationInterface* intfc,
	std::set<UInt32>& out, const char* label, UInt32 recordLength, UInt32 version)
{
	UInt32 count = 0;
	intfc->ReadRecordData(&count, sizeof(count));

	// Bounds-check count against the record length supplied by GetNextRecordInfo.
	// A corrupt or hand-edited cosave could otherwise request billions of reads.
	UInt32 maxCount = (recordLength > sizeof(UInt32))
		? (recordLength - sizeof(UInt32)) / sizeof(UInt32)
		: 0;
	if (count > maxCount)
	{
		_MESSAGE("UnreadNotes: WARNING — %s record claims %u entries but only %u fit in %u bytes; clamping",
			label, count, maxCount, recordLength);
		count = maxCount;
	}

	UInt32 loaded = 0;
	for (UInt32 i = 0; i < count; i++)
	{
		UInt32 savedFormID = 0, resolvedFormID = 0;
		intfc->ReadRecordData(&savedFormID, sizeof(savedFormID));
		if (intfc->ResolveFormId(savedFormID, &resolvedFormID))
		{
			out.insert(resolvedFormID);
			loaded++;
		}
		else
		{
			LOG(1, "UnreadNotes: %s FormID %08X could not be resolved, skipping",
				label, savedFormID);
		}
	}
	LOG(0, "UnreadNotes: Load — %u of %u %s entries resolved (record v%u)",
		loaded, count, label, version);
}

void Serialization_Revert(const F4SESerializationInterface* intfc)
{
	LOG(1, "UnreadNotes: Revert — clearing %u read + %u marked",
		static_cast<UInt32>(g_readNotes.size()),
		static_cast<UInt32>(g_markedNotes.size()));
	g_readNotes.clear();
	g_markedNotes.clear();
}

void Serialization_Save(const F4SESerializationInterface* intfc)
{
	LOG(1, "UnreadNotes: Save — writing %u read + %u marked",
		static_cast<UInt32>(g_readNotes.size()),
		static_cast<UInt32>(g_markedNotes.size()));
	WriteFormIDSet(intfc, kRecordType_ReadNotes,   g_readNotes);
	WriteFormIDSet(intfc, kRecordType_MarkedNotes, g_markedNotes);
}

void Serialization_Load(const F4SESerializationInterface* intfc)
{
	UInt32 type, version, length;

	while (intfc->GetNextRecordInfo(&type, &version, &length))
	{
		if (type == kRecordType_ReadNotes)
		{
			if (version == kDataVersion)
				ReadFormIDSet(intfc, g_readNotes, "read", length, version);
			else
				_MESSAGE("UnreadNotes: WARNING — ReadNotes v%u unknown, skipping", version);
		}
		else if (type == kRecordType_MarkedNotes)
		{
			if (version == kDataVersion)
				ReadFormIDSet(intfc, g_markedNotes, "marked", length, version);
			else
				_MESSAGE("UnreadNotes: WARNING — MarkedNotes v%u unknown, skipping", version);
		}
		// Unknown record types fall through silently — preserves forward-compat
		// if a future version adds new records that this loader doesn't recognise.
	}

	// Enforce mutual-exclusion invariant. A corrupt or hand-edited cosave could
	// list the same FormID in both sets; marked takes priority in the UI, so
	// prefer that state for consistency.
	UInt32 pruned = 0;
	for (UInt32 id : g_markedNotes)
		pruned += static_cast<UInt32>(g_readNotes.erase(id));
	if (pruned > 0)
		_MESSAGE("UnreadNotes: WARNING — %u FormID(s) in both read and marked sets; "
			"removed from read (marked takes priority)", pruned);
}


// ============================================================================
// Scaleform Functions (accessible from Flash as f4se.plugins.UnreadNotes.*)
// ============================================================================

class ScaleformGetVersion : public GFxFunctionHandler
{
public:
	void Invoke(Args* args) override
	{
		args->result->SetUInt(2);
	}
};

class ScaleformIsNoteRead : public GFxFunctionHandler
{
public:
	void Invoke(Args* args) override
	{
		if (args->numArgs < 1) { args->result->SetBool(false); return; }
		UInt32 formID = args->args[0].GetUInt();
		args->result->SetBool(g_readNotes.count(formID) > 0);
	}
};

class ScaleformGetReadCount : public GFxFunctionHandler
{
public:
	void Invoke(Args* args) override
	{
		args->result->SetUInt(static_cast<UInt32>(g_readNotes.size()));
	}
};

// NOLINTNEXTLINE(readability-non-const-parameter) — signature fixed by F4SE's Register API
bool ScaleformCallback(GFxMovieView* view, GFxValue* value)
{
	GFxMovieRoot* movieRoot = view->movieRoot;
	RegisterFunction<ScaleformGetVersion>(value, movieRoot, "GetVersion");
	RegisterFunction<ScaleformIsNoteRead>(value, movieRoot, "IsNoteRead");
	RegisterFunction<ScaleformGetReadCount>(value, movieRoot, "GetReadCount");
	return true;
}


// ============================================================================
// Pip-Boy Helpers
// ============================================================================

static GFxMovieRoot* GetPipboyMovieRoot()
{
	static BSFixedString pipboyName("PipboyMenu");
	if (!(*g_ui)->IsMenuOpen(pipboyName))
		return nullptr;

	IMenu* menu = (*g_ui)->GetMenu(pipboyName);
	if (!menu || !menu->movie || !menu->movie->movieRoot)
		return nullptr;

	return menu->movie->movieRoot;
}

struct ReadableItemInfo
{
	UInt32 formID = 0;
	UInt32 filterFlag = 0;
	char   tag[32]   = {};   // FallUI _tagStr (e.g. "HolotapeT"); empty if absent
	char   name[128] = {};   // textClean (display name without [Tag] prefix)
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

static bool GetSelectedReadableItem(const GFxMovieRoot* movieRoot, ReadableItemInfo& out,
	UInt32 filterMask = kFilterMask_ReadableItems)
{
	GFxValue selectedEntry;
	if (!movieRoot->GetVariable(&selectedEntry, "root.Menu_mc.CurrentPage.List_mc.selectedEntry"))
		return false;

	if (!selectedEntry.IsObject())
		return false;

	GFxValue filterFlagVal, formIDVal;
	if (!selectedEntry.HasMember("filterFlag") || !selectedEntry.GetMember("filterFlag", &filterFlagVal))
		return false;

	UInt32 filterFlag = filterFlagVal.GetUInt();
	if (!(filterFlag & filterMask))
		return false;

	if (!selectedEntry.HasMember("formID") || !selectedEntry.GetMember("formID", &formIDVal))
		return false;

	out.formID     = formIDVal.GetUInt();
	out.filterFlag = filterFlag;

	// FallUI's Parser.as pulls the leading [Token] off the item name into _tagStr
	// and stores the remainder in textClean. Both missing on untagged items or
	// when FallUI isn't installed — harmless, we fall back to filterFlag/formID.
	GFxValue tagStrVal;
	if (selectedEntry.HasMember("_tagStr") &&
		selectedEntry.GetMember("_tagStr", &tagStrVal) &&
		tagStrVal.GetType() == GFxValue::kType_String)
	{
		strncpy_s(out.tag, tagStrVal.GetString(), sizeof(out.tag) - 1);
	}

	GFxValue nameVal;
	if (selectedEntry.HasMember("textClean") &&
		selectedEntry.GetMember("textClean", &nameVal) &&
		nameVal.GetType() == GFxValue::kType_String)
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
static const char* ExpectedSuffixForFormID(UInt32 formID)
{
	if (g_markedNotes.count(formID) > 0) return g_cfgMarkSuffix;
	if (g_readNotes.count(formID)   > 0) return g_cfgSuffix;
	return "";
}

// Fast path: returns true if any item's text doesn't match its state.
// Runs every frame but only does simple string compares.
static bool QuickCheckEntriesNeedModification(GFxValue& entryList, UInt32 entryCount)
{
	if (!g_cfgSuffix[0] && !g_cfgMarkSuffix[0])
		return false;  // No suffixes configured — nothing to apply

	for (UInt32 idx = 0; idx < entryCount; idx++)
	{
		GFxValue de;
		entryList.GetElement(idx, &de);
		if (!de.IsObject()) continue;

		GFxValue ffv;
		if (!de.HasMember("filterFlag") || !de.GetMember("filterFlag", &ffv))
			continue;
		if (!(ffv.GetUInt() & kFilterMask_MarkableItems))
			continue;

		GFxValue fiv;
		if (!de.HasMember("formID") || !de.GetMember("formID", &fiv))
			continue;

		const char* expected = ExpectedSuffixForFormID(fiv.GetUInt());
		if (!expected[0])
			continue;  // Item has no state requiring a suffix

		GFxValue tv;
		if (!de.HasMember("text") || !de.GetMember("text", &tv) ||
			tv.GetType() != GFxValue::kType_String)
			continue;

		const char* t = tv.GetString();
		size_t tLen = strlen(t);
		size_t sLen = strlen(expected);
		if (tLen < sLen || strcmp(t + tLen - sLen, expected) != 0)
			return true;  // Expected suffix missing
	}
	return false;
}

static int ModifyEntryListData(GFxMovieRoot* movieRoot, GFxValue& entryList, UInt32 entryCount)
{
	int modified = 0;

	for (UInt32 idx = 0; idx < entryCount; idx++)
	{
		GFxValue dataEntry;
		entryList.GetElement(idx, &dataEntry);
		if (!dataEntry.IsObject()) continue;

		GFxValue filterFlagVal;
		if (!dataEntry.HasMember("filterFlag") ||
			!dataEntry.GetMember("filterFlag", &filterFlagVal))
			continue;

		UInt32 filterFlag = filterFlagVal.GetUInt();
		if (!(filterFlag & kFilterMask_MarkableItems))
			continue;

		GFxValue formIDVal;
		if (!dataEntry.HasMember("formID") ||
			!dataEntry.GetMember("formID", &formIDVal))
			continue;

		UInt32 formID = formIDVal.GetUInt();

		// Debug: mark all readable items as read
		if (g_markAllReadPending && formID != 0)
			g_readNotes.insert(formID);

		const char* suffix = ExpectedSuffixForFormID(formID);
		if (!suffix[0])
			continue;  // No state — nothing to apply

		GFxValue textVal;
		if (!dataEntry.HasMember("text") ||
			!dataEntry.GetMember("text", &textVal) ||
			textVal.GetType() != GFxValue::kType_String)
			continue;

		const char* text = textVal.GetString();
		size_t tLen = strlen(text);
		size_t sLen = strlen(suffix);

		// Skip if the correct suffix is already in place
		if (tLen >= sLen && strcmp(text + tLen - sLen, suffix) == 0)
			continue;

		char buf[512];
		snprintf(buf, sizeof(buf), "%s%s", text, suffix);

		GFxValue newTextVal;
		movieRoot->CreateString(&newTextVal, buf);
		dataEntry.SetMember("text", &newTextVal);
		modified++;
	}

	if (g_markAllReadPending)
	{
		LOG(0, "UnreadNotes: DEBUG — Marked all readable items as read (total: %u)",
			static_cast<UInt32>(g_readNotes.size()));
		g_markAllReadPending = false;
	}

	return modified;
}


// Strip known read or mark suffix from the entry's text if present.
// Called after a state transition so the list refresh shows a clean name
// before ModifyEntryListData re-applies whichever suffix the new state needs.
static void StripKnownSuffixesFromEntry(GFxMovieRoot* movieRoot, GFxValue& entry,
	const char* itemLabel)
{
	GFxValue textVal;
	if (!entry.HasMember("text") || !entry.GetMember("text", &textVal) ||
		textVal.GetType() != GFxValue::kType_String)
		return;

	const char* t = textVal.GetString();
	size_t tLen = strlen(t);

	const char* candidates[2] = { g_cfgSuffix, g_cfgMarkSuffix };
	for (const char* s : candidates)
	{
		size_t sLen = strlen(s);
		if (sLen == 0 || tLen < sLen) continue;
		if (strcmp(t + tLen - sLen, s) != 0) continue;

		char buf[512];
		snprintf(buf, sizeof(buf), "%.*s", static_cast<int>(tLen - sLen), t);
		GFxValue clean;
		movieRoot->CreateString(&clean, buf);
		entry.SetMember("text", &clean);
		LOG(2, "UnreadNotes: Stripped \"%s\" from \"%s\"", s, itemLabel);
		return;
	}
}


// ============================================================================
// Input Handler — toggle read/unread and mark/unmark
// ============================================================================
// Registered with MenuControls::inputEvents. Handles two configurable keys:
//   iToggleKey — cycles read/unread. Clears marked state (mutual exclusion).
//   iMarkKey   — cycles marked/unmarked. Clears read state.
// An item is always in exactly one of three states: unread, read, or marked.
// After any transition we strip whatever suffix was present and kick an
// InvalidateData so the display catches up immediately.
class UnreadNotesInputHandler : public BSInputEventUser
{
public:
	UnreadNotesInputHandler() : BSInputEventUser(true) {}
	~UnreadNotesInputHandler() override = default;

	void OnButtonEvent(ButtonEvent* inputEvent) override
	{
		if (!inputEvent)
			return;
		if (inputEvent->deviceType != InputEvent::kDeviceType_Keyboard)
			return;

		UInt32 key = inputEvent->keyMask;
		bool isToggle = (g_cfgToggleKey != 0 && key == g_cfgToggleKey);
		bool isMark   = (g_cfgMarkKey   != 0 && key == g_cfgMarkKey);
		if (!isToggle && !isMark)
			return;

		// Initial press only — ignore hold-repeat and release.
		if (inputEvent->isDown != 1.0f || inputEvent->timer != 0.0f)
			return;

		GFxMovieRoot* movieRoot = GetPipboyMovieRoot();
		if (!movieRoot)
		{
			LOG(2, "UnreadNotes: Key pressed but Pip-Boy not open");
			return;
		}

		ReadableItemInfo info;
		if (!GetSelectedReadableItem(movieRoot, info, kFilterMask_MarkableItems))
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

		LOG(1, "UnreadNotes: Toggled %s %s \"%s\" (FormID %08X) (read=%u marked=%u)",
			direction, GetItemTypeLabel(info), info.name, info.formID,
			static_cast<UInt32>(g_readNotes.size()),
			static_cast<UInt32>(g_markedNotes.size()));

		// Strip whichever suffix was present; ModifyEntryListData on the next
		// frame will re-apply the correct one for the new state (if any).
		GFxValue selectedEntry;
		if (movieRoot->GetVariable(&selectedEntry,
				"root.Menu_mc.CurrentPage.List_mc.selectedEntry") &&
			selectedEntry.IsObject())
		{
			StripKnownSuffixesFromEntry(movieRoot, selectedEntry, info.name);
		}

		// Refresh the list so suffix changes are visible immediately.
		// Alpha dimming updates naturally via AdvanceMovie on the next frame.
		GFxValue listMc;
		if (movieRoot->GetVariable(&listMc, "root.Menu_mc.CurrentPage.List_mc") &&
			listMc.IsObject())
		{
			listMc.Invoke("InvalidateData", nullptr, nullptr, 0);
		}
	}
};

static UnreadNotesInputHandler g_inputHandler;


// ============================================================================
// Menu Event Handler
// ============================================================================
class MenuEventHandler : public BSTEventSink<MenuOpenCloseEvent>
{
public:
	~MenuEventHandler() override = default;

	EventResult ReceiveEvent(MenuOpenCloseEvent* evn, void* dispatcher) override
	{
		const char* name = evn->menuName.c_str();

		// MarkAsRead: detect note/holotape activation from Pip-Boy
		if (evn->isOpen)
		{
			bool isBookMenu = (strcmp(name, "BookMenu") == 0);
			bool isTerminalMenu = (strcmp(name, "TerminalMenu") == 0);

			if (isBookMenu || isTerminalMenu)
			{
				if (GFxMovieRoot* movieRoot = GetPipboyMovieRoot())
				{
					ReadableItemInfo info;
					if (GetSelectedReadableItem(movieRoot, info))
					{
						if (g_markedNotes.count(info.formID) > 0)
						{
							LOG(2, "UnreadNotes: Auto-mark skipped for %s \"%s\" (FormID %08X) — item is marked",
								GetItemTypeLabel(info), info.name, info.formID);
						}
						else if ([[maybe_unused]] bool isNew = g_readNotes.insert(info.formID).second)
						{
							LOG(1, "UnreadNotes: Marked %s \"%s\" (FormID %08X) as read via %s (total: %u)",
								GetItemTypeLabel(info), info.name, info.formID, name,
								static_cast<UInt32>(g_readNotes.size()));
						}
					}
				}
			}
		}

		// Hot-reload config on each Pip-Boy open
		if (evn->isOpen && strcmp(name, "PipboyMenu") == 0)
		{
			LoadConfig();
			LOG(1, "UnreadNotes: PipboyMenu opened");
		}

		return kEvent_Continue;
	}
};

static MenuEventHandler g_menuEventHandler;


// ============================================================================
// F4SE Message Handler
// ============================================================================
// NOLINTNEXTLINE(readability-non-const-parameter) — signature fixed by F4SE's EventCallback typedef
void OnF4SEMessage(F4SEMessagingInterface::Message* msg)
{
	if (msg->type == F4SEMessagingInterface::kMessage_GameDataReady)
	{
		// F4SE packs the bool directly into the data pointer field (not a pointer to a bool)
		if (bool isReady = msg->data != nullptr; isReady && *g_ui)
		{
			(*g_ui)->menuOpenCloseEventSource.AddEventSink(&g_menuEventHandler);
			LOG(0, "UnreadNotes: Menu event handler registered");

			if (*g_menuControls)
			{
				auto& arr = (*g_menuControls)->inputEvents;
				UInt32 before = arr.count;
				bool ok = arr.Push(&g_inputHandler);
				LOG(0, "UnreadNotes: Input handler registered (toggleKey=%u, arr %u→%u, push=%d)",
					g_cfgToggleKey, before, arr.count, ok ? 1 : 0);
			}
			else
			{
				_MESSAGE("UnreadNotes: WARNING — g_menuControls null, toggle-key feature disabled");
			}
		}
	}
}


// ============================================================================
// AdvanceMovie Hook
// ============================================================================
// Hooks GameMenuBase::Impl_AdvanceMovie to run our data modifications inside
// the game's per-frame menu update. This is the key to making changes display
// immediately — UITask changes happen after Scaleform commits the frame,
// but AdvanceMovie runs during the frame update.

using AdvanceMovie_Fn = void (*)(GameMenuBase*, float, void*);
AdvanceMovie_Fn AdvanceMovie_Original = nullptr;
RelocAddr<uintptr_t> AdvanceMovie_Addr(0x0210EED0);

// Detect a holotape starting playback via the PipboyMenu.DataObj.HolotapePlaying
// flag (public Flash field populated by the game via PopulatePipboyInfoObj).
// Runs every frame the Pipboy is open. Edge-detection only: we mark on the
// false→true transition, NOT on the level. This is essential because an audio
// holotape keeps playing while the player navigates the Pipboy (and even after
// they close it), and we must not keep marking whichever item they hover.
static void DetectHolotapePlayback(const GFxMovieRoot * movieRoot)
{
	static bool s_prevPlaying = false;
	static bool s_initialized = false;

	GFxValue playingVal;
	bool currentPlaying = false;
	if (movieRoot->GetVariable(&playingVal, "root.Menu_mc.DataObj.HolotapePlaying"))
		currentPlaying = playingVal.GetBool();

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
	bool haveSelection = GetSelectedReadableItem(movieRoot, info);

	LOG(2, "UnreadNotes: Holotape transition %s→%s — selected: %s%08X",
		s_prevPlaying  ? "true"  : "false",
		currentPlaying ? "true"  : "false",
		haveSelection  ? ""         : "(none) ",
		haveSelection  ? info.formID : 0);

	// Mark only on false→true. Each new holotape produces its own transition
	// cycle: the tape-loading animation briefly sets HolotapePlaying=false
	// before the new audio starts, so seamless play-next-without-stopping
	// still generates a detectable edge.
	if (currentPlaying && !s_prevPlaying && haveSelection)
	{
		if (g_markedNotes.count(info.formID) > 0)
		{
			LOG(2, "UnreadNotes: Auto-mark skipped for %s \"%s\" (FormID %08X) — item is marked",
				GetItemTypeLabel(info), info.name, info.formID);
		}
		else if (g_readNotes.insert(info.formID).second)
		{
			LOG(1, "UnreadNotes: Marked %s \"%s\" (FormID %08X) as read via HolotapePlaying (total: %u)",
				GetItemTypeLabel(info), info.name, info.formID,
				static_cast<UInt32>(g_readNotes.size()));
		}
	}

	s_prevPlaying = currentPlaying;
}

void AdvanceMovie_Hook(GameMenuBase* menu, float unk0, void* unk1)
{
	// Call original first — lets FallUI do its normal rendering
	AdvanceMovie_Original(menu, unk0, unk1);

	// Check this is PipboyMenu (runs for every menu's AdvanceMovie, not just us)
	if (!menu->movie || !menu->movie->movieRoot)
		return;

	static BSFixedString pipboyName("PipboyMenu");
	if (!(*g_ui)->IsMenuOpen(pipboyName))
		return;

	IMenu* pipMenu = (*g_ui)->GetMenu(pipboyName);
	if (static_cast<IMenu*>(menu) != pipMenu)
		return;

	GFxMovieRoot* movieRoot = menu->movie->movieRoot;

	// Holotape playback detection — runs every Pipboy frame regardless of
	// g_readNotes state (we need to catch the very first play of a session).
	DetectHolotapePlayback(movieRoot);

	// Visual work only runs when we actually have something to show
	if (g_readNotes.empty() && !g_markAllReadPending)
		return;

	LARGE_INTEGER perfStart, perfEnd;
	QueryPerformanceCounter(&perfStart);

	GFxValue entryList;
	if (!movieRoot->GetVariable(&entryList, kEntryListPath) || !entryList.IsArray())
		return;

	UInt32 entryCount = entryList.GetArraySize();
	if (entryCount == 0) return;

	// Fast path: only run the full modification walk when something actually
	// needs changing. QuickCheck is O(n) but cheap; ModifyEntryListData does
	// the full work (CreateString, SetMember, InvalidateData) that we want to
	// avoid on frames where FallUI has already picked up our changes.
	bool needsTextMods = g_markAllReadPending ||
		QuickCheckEntriesNeedModification(entryList, entryCount);

	if (needsTextMods)
	{
		int modified = ModifyEntryListData(movieRoot, entryList, entryCount);

		if (modified > 0)
		{
			GFxValue listMc;
			if (movieRoot->GetVariable(&listMc, "root.Menu_mc.CurrentPage.List_mc") &&
				listMc.IsObject())
			{
				listMc.Invoke("InvalidateData", nullptr, nullptr, 0);
			}

			LOG(1, "UnreadNotes: AdvanceMovie — modified %d entries", modified);
		}
	}

	// Apply renderer alpha dimming — dims the entire row including item counts.
	// Runs every frame to handle renderer recycling across subcategory switches.
	// This is lightweight: 14 renderers × (getChildAt + itemIndex lookup + alpha set).
	static const char* kEntryHolderPath =
		"root.Menu_mc.CurrentPage.List_mc.entryHolder_mc";

	GFxValue entryHolder;
	if (!movieRoot->GetVariable(&entryHolder, kEntryHolderPath) || !entryHolder.IsObject())
		return;

	char pathBuf[256];
	snprintf(pathBuf, sizeof(pathBuf), "%s.numChildren", kEntryHolderPath);
	GFxValue numRendVal;
	if (!movieRoot->GetVariable(&numRendVal, pathBuf))
		return;
	auto numRenderers = static_cast<int>(numRendVal.GetNumber());

	for (int i = 0; i < numRenderers && i < 50; i++)
	{
		GFxValue indexArg;
		indexArg.SetInt(i);
		GFxValue renderer;
		if (!entryHolder.Invoke("getChildAt", &renderer, &indexArg, 1))
			continue;
		if (!renderer.IsObject()) continue;

		GFxValue itemIndexVal;
		if (!renderer.HasMember("itemIndex") ||
			!renderer.GetMember("itemIndex", &itemIndexVal))
			continue;

		auto itemIndex = static_cast<int>(itemIndexVal.GetNumber());
		if (itemIndex < 0 || itemIndex >= static_cast<int>(entryCount))
			continue;

		GFxValue dataEntry;
		entryList.GetElement(itemIndex, &dataEntry);
		if (!dataEntry.IsObject()) continue;

		// Determine target alpha based on state.
		//   Marked → full alpha (bright, attention-grabbing).
		//   Read   → configured brightness (dimmed).
		//   Unread → full alpha.
		double targetAlpha = 1.0;

		GFxValue filterFlagVal;
		if (dataEntry.HasMember("filterFlag") &&
			dataEntry.GetMember("filterFlag", &filterFlagVal))
		{
			UInt32 filterFlag = filterFlagVal.GetUInt();
			if (filterFlag & kFilterMask_MarkableItems)
			{
				GFxValue formIDVal;
				if (dataEntry.HasMember("formID") &&
					dataEntry.GetMember("formID", &formIDVal))
				{
					UInt32 fid = formIDVal.GetUInt();
					if (g_markedNotes.count(fid) > 0)
						targetAlpha = 1.0;
					else if (g_readNotes.count(fid) > 0)
						targetAlpha = static_cast<double>(g_cfgBrightness);
				}
			}
		}

		GFxValue alpha;
		alpha.SetNumber(targetAlpha);
		renderer.SetMember("alpha", &alpha);
	}

	// Performance logging — log every 300th frame (~5s at 60fps) at level 2
	QueryPerformanceCounter(&perfEnd);

	static const LARGE_INTEGER s_perfFreq = []{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return f;
	}();
	double microseconds = static_cast<double>(perfEnd.QuadPart - perfStart.QuadPart) * 1000000.0 / static_cast<double>(s_perfFreq.QuadPart);

	static int s_frameCount = 0;
	static double s_totalUs = 0;
	static double s_maxUs = 0;
	s_frameCount++;
	s_totalUs += microseconds;
	if (microseconds > s_maxUs) s_maxUs = microseconds;

	if (s_frameCount >= 300)
	{
		LOG(2, "UnreadNotes: Perf — avg=%.1fus max=%.1fus over %d frames",
			s_totalUs / s_frameCount, s_maxUs, s_frameCount);
		s_frameCount = 0;
		s_totalUs = 0;
		s_maxUs = 0;
	}
}

static void InstallAdvanceMovieHook()
{
	auto* funcStart = reinterpret_cast<UInt8*>(AdvanceMovie_Addr.GetUIntPtr());
	LOG(2, "UnreadNotes: AdvanceMovie at %p, first bytes: %02X %02X %02X %02X %02X %02X",
		funcStart, funcStart[0], funcStart[1], funcStart[2],
		funcStart[3], funcStart[4], funcStart[5]);

	struct AdvanceMovie_Code : Xbyak::CodeGenerator
	{
		AdvanceMovie_Code(void* buf, uintptr_t origAddr) : Xbyak::CodeGenerator(4096, buf)
		{
			Xbyak::Label retnLabel;

			// Reproduce original function prologue (first 6 bytes replaced by hook)
			auto* src = reinterpret_cast<UInt8*>(origAddr);
			for (int i = 0; i < 6; i++)
				db(src[i]);

			// Jump back to original function after hooked bytes
			jmp(ptr[rip + retnLabel]);

			L(retnLabel);
			dq(origAddr + 6);
		}
	};

	void* codeBuf = g_localTrampoline.StartAlloc();
	AdvanceMovie_Code code(codeBuf, AdvanceMovie_Addr.GetUIntPtr());
	g_localTrampoline.EndAlloc(code.getCurr());

	AdvanceMovie_Original = reinterpret_cast<AdvanceMovie_Fn>(codeBuf);

	g_branchTrampoline.Write6Branch(AdvanceMovie_Addr.GetUIntPtr(),
		reinterpret_cast<uintptr_t>(AdvanceMovie_Hook));

	LOG(0, "UnreadNotes: AdvanceMovie hook installed");
}


// ============================================================================
// Plugin Entry Points
// ============================================================================
extern "C"
{

__declspec(dllexport) bool F4SEPlugin_Query(const F4SEInterface* f4se, PluginInfo* info)
{
	IDebugLog::OpenRelative(CSIDL_MYDOCUMENTS, R"(\My Games\Fallout4\F4SE\UnreadNotes.log)");

	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "UnreadNotes";
	info->version = 1;

	if (f4se->runtimeVersion != RUNTIME_VERSION_1_10_163)
	{
		_FATALERROR("UnreadNotes: unsupported runtime version %08X (expected %08X)",
			f4se->runtimeVersion, RUNTIME_VERSION_1_10_163);
		return false;
	}

	return true;
}

__declspec(dllexport) bool F4SEPlugin_Load(const F4SEInterface* f4se)
{
	_MESSAGE("UnreadNotes v1.2.0: loading");

	LoadConfig();

	g_pluginHandle = f4se->GetPluginHandle();

	// --- Scaleform ---
	auto* scaleform = static_cast<F4SEScaleformInterface*>(f4se->QueryInterface(kInterface_Scaleform));
	if (!scaleform)
	{
		_FATALERROR("UnreadNotes: couldn't get Scaleform interface");
		return false;
	}
	scaleform->Register("UnreadNotes", ScaleformCallback);

	// --- Serialization ---
	auto* serialization = static_cast<F4SESerializationInterface*>(f4se->QueryInterface(kInterface_Serialization));
	if (!serialization)
	{
		_FATALERROR("UnreadNotes: couldn't get Serialization interface");
		return false;
	}
	serialization->SetUniqueID(g_pluginHandle, kPluginUID);
	serialization->SetRevertCallback(g_pluginHandle, Serialization_Revert);
	serialization->SetSaveCallback(g_pluginHandle, Serialization_Save);
	serialization->SetLoadCallback(g_pluginHandle, Serialization_Load);

	// --- Messaging ---
	g_messaging = static_cast<F4SEMessagingInterface*>(f4se->QueryInterface(kInterface_Messaging));
	if (!g_messaging)
	{
		_FATALERROR("UnreadNotes: couldn't get Messaging interface");
		return false;
	}
	g_messaging->RegisterListener(g_pluginHandle, "F4SE", OnF4SEMessage);

	// --- AdvanceMovie Hook ---
	if (!g_branchTrampoline.Create(64))
	{
		_MESSAGE("UnreadNotes: WARNING — couldn't create branch trampoline");
	}
	else if (!g_localTrampoline.Create(128))
	{
		_MESSAGE("UnreadNotes: WARNING — couldn't create local trampoline");
	}
	else
	{
		InstallAdvanceMovieHook();
	}

	_MESSAGE("UnreadNotes v1.2.0: loaded successfully");

	return true;
}

}  // extern "C"

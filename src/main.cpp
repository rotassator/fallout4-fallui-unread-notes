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
#include "xbyak/xbyak.h"

#include <shlobj.h>
#include <set>

static PluginHandle g_pluginHandle = kPluginHandle_Invalid;
static F4SEMessagingInterface* g_messaging = nullptr;
static F4SETaskInterface* g_taskInterface = nullptr;


// ============================================================================
// Read Tracking Data
// ============================================================================
static std::set<UInt32> g_readNotes;

// Note/holotape filter mask: notes (0x80) | holotapes (0x2000)
// Misc notes (0x200) excluded — recipes/schematics/contracts aren't readable
static constexpr UInt32 kFilterMask_ReadableItems = 0x2080;

// Path to the inventory data array in the PipboyMenu Scaleform movie
static const char* kEntryListPath =
	"root.Menu_mc.CurrentPage.List_mc.entryList";


// ============================================================================
// Configuration (loaded from Data/F4SE/Plugins/UnreadNotes.ini)
// ============================================================================
static int    g_cfgLogLevel = 1;
static float  g_cfgBrightness = 0.5f;
static char   g_cfgSuffix[64] = " (Read)";
static bool   g_cfgSortReadToBottom = true;
static bool   g_markAllReadPending = false;

// Log macro: only logs if current level >= required level
// 0 = minimal (errors, startup, config), 1 = normal, 2 = debug (perf, per-item)
#define LOG(level, ...) do { if (g_cfgLogLevel >= level) _MESSAGE(__VA_ARGS__); } while(0)

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
		"; Logging level. 0 = minimal, 1 = normal, 2 = debug (includes perf stats).\n"
		"; Default: 1\n"
		"iLogLevel=1\n"
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
		"; Sort read items below unread items in the list.\n"
		"; 0 = keep original sort order, 1 = read items sort to bottom.\n"
		"; Default: 1\n"
		"bSortReadToBottom=1\n"
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
	char iniPath[MAX_PATH];
	snprintf(iniPath, sizeof(iniPath), R"(Data\F4SE\Plugins\UnreadNotes.ini)");

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

	// Sanitise suffix: strip < > which break FallUI's HTML text mode.
	{
		char sanitised[64] = {};
		int j = 0;
		for (int i = 0; suffixBuf[i] && j < static_cast<int>(sizeof(sanitised)) - 1; i++)
		{
			if (suffixBuf[i] != '<' && suffixBuf[i] != '>')
				sanitised[j++] = suffixBuf[i];
		}
		sanitised[j] = '\0';

		if (j != static_cast<int>(strlen(suffixBuf)))
			_MESSAGE("UnreadNotes: WARNING — stripped < > from sSuffix: \"%s\" -> \"%s\"",
				suffixBuf, sanitised);

		strncpy_s(g_cfgSuffix, sanitised, sizeof(g_cfgSuffix) - 1);
	}

	g_cfgSortReadToBottom = GetPrivateProfileIntA("Display", "bSortReadToBottom", 1, iniPath) != 0;

	LOG(0, "UnreadNotes: Config — brightness=%d%% suffix=\"%s\" sort=%d logLevel=%d",
		static_cast<int>(g_cfgBrightness * 100), g_cfgSuffix, g_cfgSortReadToBottom, g_cfgLogLevel);

	// Debug commands — triggered via INI, auto-reset after use
	if (GetPrivateProfileIntA("Debug", "bResetAll", 0, iniPath) != 0)
	{
		_MESSAGE("UnreadNotes: DEBUG — Clearing all %u read notes", g_readNotes.size());
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
// 'UNrd' and 'RdNt' — do not change without migrating existing save data.
static constexpr UInt32 kPluginUID         = 0x554E7264;  // 'UNrd'
static constexpr UInt32 kRecordType_ReadNotes = 0x52644E74;  // 'RdNt'
static constexpr UInt32 kDataVersion       = 1;

void Serialization_Revert(const F4SESerializationInterface* intfc)
{
	LOG(1, "UnreadNotes: Revert — clearing %u read notes", g_readNotes.size());
	g_readNotes.clear();
}

void Serialization_Save(const F4SESerializationInterface* intfc)
{
	LOG(1, "UnreadNotes: Save — writing %u read notes", g_readNotes.size());

	if (intfc->OpenRecord(kRecordType_ReadNotes, kDataVersion))
	{
		auto count = static_cast<UInt32>(g_readNotes.size());
		intfc->WriteRecordData(&count, sizeof(count));

		for (UInt32 formID : g_readNotes)
		{
			intfc->WriteRecordData(&formID, sizeof(formID));
		}
	}
}

void Serialization_Load(const F4SESerializationInterface* intfc)
{
	UInt32 type, version, length;

	while (intfc->GetNextRecordInfo(&type, &version, &length))
	{
		if (type == kRecordType_ReadNotes)
		{
			if (version == kDataVersion)
			{
				UInt32 count = 0;
				intfc->ReadRecordData(&count, sizeof(count));

				UInt32 loaded = 0;
				for (UInt32 i = 0; i < count; i++)
				{
					UInt32 savedFormID = 0;
					intfc->ReadRecordData(&savedFormID, sizeof(savedFormID));

					UInt32 resolvedFormID = 0;
					if (intfc->ResolveFormId(savedFormID, &resolvedFormID))
					{
						g_readNotes.insert(resolvedFormID);
						loaded++;
					}
					else
					{
						LOG(1, "UnreadNotes: FormID %08X could not be resolved, skipping",
							savedFormID);
					}
				}

				LOG(0, "UnreadNotes: Load — %u of %u entries resolved", loaded, count);
			}
			else
			{
				_WARNING("UnreadNotes: unknown data version %u, skipping", version);
			}
		}
	}
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
	BSFixedString pipboyName("PipboyMenu");
	if (!(*g_ui)->IsMenuOpen(pipboyName))
		return nullptr;

	IMenu* menu = (*g_ui)->GetMenu(pipboyName);
	if (!menu || !menu->movie || !menu->movie->movieRoot)
		return nullptr;

	return menu->movie->movieRoot;
}

static bool GetSelectedReadableItem(const GFxMovieRoot* movieRoot, UInt32& formIDOut)
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
	if (!(filterFlag & kFilterMask_ReadableItems))
		return false;

	if (!selectedEntry.HasMember("formID") || !selectedEntry.GetMember("formID", &formIDVal))
		return false;

	formIDOut = formIDVal.GetUInt();
	return true;
}


// ============================================================================
// Entry List Data Modification
// ============================================================================
// Modifies the text and sort key on entryList data entries for read items.
// Called from the AdvanceMovie hook (runs inside the game's render cycle).

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
		if (!(filterFlag & kFilterMask_ReadableItems))
			continue;

		GFxValue formIDVal;
		if (!dataEntry.HasMember("formID") ||
			!dataEntry.GetMember("formID", &formIDVal))
			continue;

		UInt32 formID = formIDVal.GetUInt();

		// Debug: mark all readable items as read
		if (g_markAllReadPending && formID != 0)
			g_readNotes.insert(formID);

		if (g_readNotes.count(formID) == 0)
			continue;

		GFxValue textVal;
		if (!dataEntry.HasMember("text") ||
			!dataEntry.GetMember("text", &textVal) ||
			textVal.GetType() != GFxValue::kType_String)
			continue;

		const char* text = textVal.GetString();

		// Skip if already modified
		if (g_cfgSuffix[0] && strstr(text, g_cfgSuffix))
			continue;

		// Build modified text — suffix only. Alpha dimming is handled
		// separately by the renderer alpha code in the AdvanceMovie hook.
		if (!g_cfgSuffix[0])
			continue;  // No suffix configured, nothing to modify

		char buf[512];
		snprintf(buf, sizeof(buf), "%s%s", text, g_cfgSuffix);

		// Modify sort key to push read items to bottom of subcategory
		if (g_cfgSortReadToBottom)
		{
			GFxValue tcVal;
			if (dataEntry.HasMember("textClean") &&
				dataEntry.GetMember("textClean", &tcVal) &&
				tcVal.GetType() == GFxValue::kType_String)
			{
				const char* tc = tcVal.GetString();
				if (tc[0] != '~')
				{
					char newClean[512];
					snprintf(newClean, sizeof(newClean), "~%s", tc);
					GFxValue ncv;
					movieRoot->CreateString(&ncv, newClean);
					dataEntry.SetMember("textClean", &ncv);
				}
			}
		}

		GFxValue newTextVal;
		movieRoot->CreateString(&newTextVal, buf);
		dataEntry.SetMember("text", &newTextVal);
		modified++;
	}

	if (g_markAllReadPending)
	{
		LOG(0, "UnreadNotes: DEBUG — Marked all readable items as read (total: %u)",
			g_readNotes.size());
		g_markAllReadPending = false;
	}

	return modified;
}


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
					UInt32 formID = 0;
					if (GetSelectedReadableItem(movieRoot, formID))
					{
						if ([[maybe_unused]] bool isNew = g_readNotes.insert(formID).second)
						{
							LOG(1, "UnreadNotes: Marked FormID %08X as read (total: %u)",
								formID, g_readNotes.size());
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
static void DetectHolotapePlayback(GFxMovieRoot* movieRoot)
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
	UInt32 formID = 0;
	bool haveSelection = GetSelectedReadableItem(movieRoot, formID);

	LOG(2, "UnreadNotes: Holotape transition %s→%s — selected: %s%08X",
		s_prevPlaying  ? "true"  : "false",
		currentPlaying ? "true"  : "false",
		haveSelection  ? ""      : "(none) ",
		haveSelection  ? formID  : 0);

	// Mark only on false→true. Each new holotape produces its own transition
	// cycle: the tape-loading animation briefly sets HolotapePlaying=false
	// before the new audio starts, so seamless play-next-without-stopping
	// still generates a detectable edge.
	if (currentPlaying && !s_prevPlaying && haveSelection)
	{
		if (g_readNotes.insert(formID).second)
		{
			LOG(1, "UnreadNotes: Marked holotape FormID %08X as read (total: %u)",
				formID, g_readNotes.size());
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

	BSFixedString pipboyName("PipboyMenu");
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

	LARGE_INTEGER perfStart, perfEnd, perfFreq;
	QueryPerformanceCounter(&perfStart);

	GFxValue entryList;
	if (!movieRoot->GetVariable(&entryList, kEntryListPath) || !entryList.IsArray())
		return;

	UInt32 entryCount = entryList.GetArraySize();
	if (entryCount == 0) return;

	// Quick check: is the first known read item already modified?
	// This runs every frame but is very fast — avoids the full walk.
	if (!g_markAllReadPending)
	{
		bool needsModification = false;
		for (UInt32 idx = 0; idx < entryCount && !needsModification; idx++)
		{
			GFxValue de;
			entryList.GetElement(idx, &de);
			if (!de.IsObject()) continue;

			GFxValue ffv;
			if (!de.HasMember("filterFlag") || !de.GetMember("filterFlag", &ffv))
				continue;
			if (!(ffv.GetUInt() & kFilterMask_ReadableItems))
				continue;

			GFxValue fiv;
			if (!de.HasMember("formID") || !de.GetMember("formID", &fiv))
				continue;
			if (g_readNotes.count(fiv.GetUInt()) == 0)
				continue;

			GFxValue tv;
			if (de.HasMember("text") && de.GetMember("text", &tv) &&
				tv.GetType() == GFxValue::kType_String)
			{
				const char* t = tv.GetString();
				if (!(g_cfgSuffix[0] && strstr(t, g_cfgSuffix)))
					needsModification = true;
			}
		}

		if (!needsModification)
			goto applyAlpha;  // Text is fine, but still need to update renderer alpha
	}

	// Apply text modifications (suffix, sort key) and refresh
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

applyAlpha:
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

		// Determine target alpha based on read status
		double targetAlpha = 1.0;

		GFxValue filterFlagVal;
		if (dataEntry.HasMember("filterFlag") &&
			dataEntry.GetMember("filterFlag", &filterFlagVal))
		{
			UInt32 filterFlag = filterFlagVal.GetUInt();
			if (filterFlag & kFilterMask_ReadableItems)
			{
				GFxValue formIDVal;
				if (dataEntry.HasMember("formID") &&
					dataEntry.GetMember("formID", &formIDVal))
				{
					if (g_readNotes.count(formIDVal.GetUInt()) > 0)
						targetAlpha = static_cast<double>(g_cfgBrightness);
				}
			}
		}

		GFxValue alpha;
		alpha.SetNumber(targetAlpha);
		renderer.SetMember("alpha", &alpha);
	}

	// Performance logging — log every 60th frame to avoid spam
	QueryPerformanceCounter(&perfEnd);
	QueryPerformanceFrequency(&perfFreq);
	double microseconds = static_cast<double>(perfEnd.QuadPart - perfStart.QuadPart) * 1000000.0 / static_cast<double>(perfFreq.QuadPart);

	static int s_frameCount = 0;
	static double s_totalUs = 0;
	static double s_maxUs = 0;
	s_frameCount++;
	s_totalUs += microseconds;
	if (microseconds > s_maxUs) s_maxUs = microseconds;

	if (s_frameCount >= 300)  // Log every ~5 seconds (TODO: gate behind iLogLevel)
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
	info->version = 2;

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
	_MESSAGE("UnreadNotes v2: loading");

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

	// --- Task ---
	g_taskInterface = static_cast<F4SETaskInterface*>(f4se->QueryInterface(kInterface_Task));
	if (!g_taskInterface)
	{
		_FATALERROR("UnreadNotes: couldn't get Task interface");
		return false;
	}

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

	_MESSAGE("UnreadNotes v2: loaded successfully");

	return true;
}

}  // extern "C"

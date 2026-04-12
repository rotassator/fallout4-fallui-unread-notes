// UnreadNotes - F4SE Plugin
// Tracks read/unread status for notes and holotapes via the Pip-Boy.
// Pure C++ runtime approach — modifies entryList data for visual indication
// and sorting, with renderer alpha dimming as immediate feedback.

#include "f4se_common/f4se_version.h"
#include "f4se_common/BranchTrampoline.h"
#include "f4se_common/Relocation.h"
#include "f4se/PluginAPI.h"
#include "common/IDebugLog.h"
#include "f4se/ScaleformCallbacks.h"
#include "f4se/GameMenus.h"
#include "f4se/GameThreads.h"
#include "xbyak/xbyak.h"

#include <shlobj.h>
#include <set>
#include <vector>
#include <algorithm>
#include <string>

// ============================================================================
// Read Tracking Data (declared early so config/debug can reference it)
// ============================================================================
static std::set<UInt32> g_readNotes;

// ============================================================================
// Configuration (loaded from Data/F4SE/Plugins/UnreadNotes.ini)
// ============================================================================
static float  g_cfgBrightness = 0.5f;         // 0.0-1.0 brightness for read items
static char   g_cfgSuffix[64] = " (Read)";   // text appended to read items
static bool   g_cfgSortReadToBottom = true;   // sort read items below unread
static char   g_cfgDimColor[8] = "";          // computed from g_cfgBrightness
static bool   g_markAllReadPending = false;   // debug: mark all readable items as read

static void ComputeDimColor()
{
	// Compute a grey hex colour from the brightness level.
	// 1.0 = white (full brightness, no change), 0.0 = black (invisible).
	// The Pip-Boy's green filter tints this, so grey becomes dim green.
	int val = (int)(255.0f * g_cfgBrightness);
	if (val < 0) val = 0;
	if (val > 255) val = 255;
	snprintf(g_cfgDimColor, sizeof(g_cfgDimColor), "#%02X%02X%02X", val, val, val);
}

static void CreateDefaultConfig(const char* path)
{
	// Create a default INI if one doesn't exist.
	// This avoids shipping the INI in the mod archive, which would
	// overwrite user customisations on mod update.
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
	_MESSAGE("UnreadNotes: Created default config at %s", path);
}

static void LoadConfig()
{
	char iniPath[MAX_PATH];
	snprintf(iniPath, sizeof(iniPath), "Data\\F4SE\\Plugins\\UnreadNotes.ini");

	// Create default config if it doesn't exist
	DWORD attrs = GetFileAttributesA(iniPath);
	if (attrs == INVALID_FILE_ATTRIBUTES)
		CreateDefaultConfig(iniPath);

	int rawBrightness = GetPrivateProfileIntA("Display", "iReadBrightness", 50, iniPath);
	if (rawBrightness < 0 || rawBrightness > 100)
	{
		_MESSAGE("UnreadNotes: WARNING — iReadBrightness=%d is out of range (0-100), clamping",
			rawBrightness);
		if (rawBrightness < 0) rawBrightness = 0;
		if (rawBrightness > 100) rawBrightness = 100;
	}
	g_cfgBrightness = rawBrightness / 100.0f;

	char suffixBuf[64] = {};
	GetPrivateProfileStringA("Display", "sSuffix", " (Read)", suffixBuf, sizeof(suffixBuf), iniPath);
	strncpy_s(g_cfgSuffix, suffixBuf, sizeof(g_cfgSuffix) - 1);

	g_cfgSortReadToBottom = GetPrivateProfileIntA("Display", "bSortReadToBottom", 1, iniPath) != 0;

	ComputeDimColor();

	_MESSAGE("UnreadNotes: Config — brightness=%d%% color=%s suffix=\"%s\" sort=%d",
		(int)(g_cfgBrightness * 100), g_cfgDimColor, g_cfgSuffix, g_cfgSortReadToBottom);

	// Debug commands — triggered via INI, auto-reset after use
	if (GetPrivateProfileIntA("Debug", "bResetAll", 0, iniPath) != 0)
	{
		_MESSAGE("UnreadNotes: DEBUG — Clearing all %u read notes", g_readNotes.size());
		g_readNotes.clear();
		WritePrivateProfileStringA("Debug", "bResetAll", "0", iniPath);
	}

	if (GetPrivateProfileIntA("Debug", "bMarkAllRead", 0, iniPath) != 0)
	{
		_MESSAGE("UnreadNotes: DEBUG — bMarkAllRead requested (will mark visible items on next dimming pass)");
		// We'll handle this in ApplyDimmingImpl by setting a flag
		WritePrivateProfileStringA("Debug", "bMarkAllRead", "0", iniPath);
		g_markAllReadPending = true;
	}
}

static PluginHandle g_pluginHandle = kPluginHandle_Invalid;
static bool g_needsDisplayRefresh = false;
static F4SEMessagingInterface* g_messaging = nullptr;
static F4SETaskInterface* g_taskInterface = nullptr;

// Note/holotape filter mask: notes (0x80) | holotapes (0x2000)
// Misc notes (0x200) excluded — recipes/schematics/contracts aren't readable
static const UInt32 kFilterMask_ReadableItems = 0x2080;

// ============================================================================
// Serialization (Cosave Persistence)
// ============================================================================
static const UInt32 kPluginUID = 'UNrd';
static const UInt32 kRecordType_ReadNotes = 'RdNt';
static const UInt32 kDataVersion = 1;

void Serialization_Revert(const F4SESerializationInterface* intfc)
{
	_MESSAGE("UnreadNotes: Revert — clearing %u read notes", g_readNotes.size());
	g_readNotes.clear();
}

void Serialization_Save(const F4SESerializationInterface* intfc)
{
	_MESSAGE("UnreadNotes: Save — writing %u read notes", g_readNotes.size());

	if (intfc->OpenRecord(kRecordType_ReadNotes, kDataVersion))
	{
		UInt32 count = (UInt32)g_readNotes.size();
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
						_MESSAGE("UnreadNotes: FormID %08X could not be resolved, skipping",
							savedFormID);
					}
				}

				_MESSAGE("UnreadNotes: Load — %u of %u entries resolved", loaded, count);
			}
			else
			{
				_WARNING("UnreadNotes: unknown data version %u, skipping", version);
			}
		}
	}
}


// ============================================================================
// Scaleform Functions
// ============================================================================

class ScaleformGetVersion : public GFxFunctionHandler
{
public:
	virtual void Invoke(Args* args)
	{
		args->result->SetUInt(2);
	}
};

class ScaleformIsNoteRead : public GFxFunctionHandler
{
public:
	static bool s_loggedFirstCall;

	virtual void Invoke(Args* args)
	{
		if (args->numArgs < 1)
		{
			args->result->SetBool(false);
			return;
		}

		UInt32 formID = args->args[0].GetUInt();
		bool isRead = g_readNotes.count(formID) > 0;
		args->result->SetBool(isRead);

		// Log first few calls to confirm addon is working
		if (!s_loggedFirstCall)
		{
			_MESSAGE("UnreadNotes: IsNoteRead called from Flash! formID=%08X isRead=%d (addon is working)",
				formID, isRead);
			s_loggedFirstCall = true;
		}
	}
};
bool ScaleformIsNoteRead::s_loggedFirstCall = false;

class ScaleformMarkAsRead : public GFxFunctionHandler
{
public:
	virtual void Invoke(Args* args)
	{
		if (args->numArgs < 1) return;

		UInt32 formID = args->args[0].GetUInt();
		bool isNew = g_readNotes.insert(formID).second;
		if (isNew)
		{
			_MESSAGE("UnreadNotes: Marked FormID %08X as read (total: %u)",
				formID, g_readNotes.size());
		}
	}
};

class ScaleformGetReadCount : public GFxFunctionHandler
{
public:
	virtual void Invoke(Args* args)
	{
		args->result->SetUInt((UInt32)g_readNotes.size());
	}
};

// ResetAll() -> void. Clears all read tracking data.
class ScaleformResetAll : public GFxFunctionHandler
{
public:
	virtual void Invoke(Args* args)
	{
		UInt32 count = (UInt32)g_readNotes.size();
		g_readNotes.clear();
		_MESSAGE("UnreadNotes: Reset all — cleared %u entries", count);
	}
};

// ResetOne(formID) -> void. Marks a single item as unread.
class ScaleformResetOne : public GFxFunctionHandler
{
public:
	virtual void Invoke(Args* args)
	{
		if (args->numArgs < 1) return;

		UInt32 formID = args->args[0].GetUInt();
		if (g_readNotes.erase(formID))
		{
			_MESSAGE("UnreadNotes: Reset FormID %08X (remaining: %u)",
				formID, g_readNotes.size());
		}
	}
};

// SetEntryTextHook — Called by FallUI's hook system during list rendering.
// FallUI calls: callHooks("BSScrollingListEntry.SetEntryText:...", renderer, dataEntry)
// We receive the renderer (display object) and the data entry (with formID, filterFlag).
// This runs at exactly the right time — during rendering, when the renderer
// is assigned its correct data. No stale state, no recycling issues.
class ScaleformSetEntryTextHook : public GFxFunctionHandler
{
public:
	static bool s_loggedFirst;

	virtual void Invoke(Args* args)
	{
		// Called by FallUI's injection handler with:
		//   handler.call(null, list, renderer, dataEntry, extraData)
		// So args[0]=list, args[1]=renderer, args[2]=dataEntry, args[3]=extraData

		if (!s_loggedFirst)
		{
			_MESSAGE("UnreadNotes: SetEntryText hook FIRED! numArgs=%d", args->numArgs);
			for (UInt32 i = 0; i < args->numArgs && i < 5; i++)
				_MESSAGE("UnreadNotes:   arg[%d] type=%d", i, args->args[i].GetType());
			s_loggedFirst = true;
		}

		if (args->numArgs < 3) return;

		GFxValue* renderer = &args->args[1];
		GFxValue* dataEntry = &args->args[2];

		if (!renderer || !dataEntry) return;
		if (!renderer->IsObject() || !dataEntry->IsObject())
			return;

		GFxValue filterFlagVal;
		if (!dataEntry->HasMember("filterFlag") ||
			!dataEntry->GetMember("filterFlag", &filterFlagVal))
			return;

		UInt32 filterFlag = filterFlagVal.GetUInt();

		if (!(filterFlag & kFilterMask_ReadableItems))
		{
			// Reset alpha on non-readable items (clears stale dimming)
			GFxValue alpha;
			alpha.SetNumber(1.0);
			renderer->SetMember("alpha", &alpha);
			return;
		}

		GFxValue formIDVal;
		if (!dataEntry->HasMember("formID") ||
			!dataEntry->GetMember("formID", &formIDVal))
			return;

		UInt32 formID = formIDVal.GetUInt();
		bool isRead = g_readNotes.count(formID) > 0;

		GFxValue alpha;
		alpha.SetNumber(isRead ? (double)g_cfgBrightness : 1.0);
		renderer->SetMember("alpha", &alpha);
	}
};
bool ScaleformSetEntryTextHook::s_loggedFirst = false;

// Global instance — kept alive for the lifetime of the plugin
static ScaleformSetEntryTextHook g_setEntryTextHook;
static bool g_hookRegistered = false;


// ============================================================================
// Scaleform Registration Callback
// ============================================================================
bool ScaleformCallback(GFxMovieView* view, GFxValue* value)
{
	GFxMovieRoot* movieRoot = view->movieRoot;

	RegisterFunction<ScaleformGetVersion>(value, movieRoot, "GetVersion");
	RegisterFunction<ScaleformIsNoteRead>(value, movieRoot, "IsNoteRead");
	RegisterFunction<ScaleformMarkAsRead>(value, movieRoot, "MarkAsRead");
	RegisterFunction<ScaleformGetReadCount>(value, movieRoot, "GetReadCount");
	RegisterFunction<ScaleformResetAll>(value, movieRoot, "ResetAll");
	RegisterFunction<ScaleformResetOne>(value, movieRoot, "ResetOne");

	return true;
}


// ============================================================================
// Pipboy Scaleform Helpers
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

static bool GetSelectedReadableItem(GFxMovieRoot* movieRoot, UInt32& formIDOut)
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
// Display Tree Diagnostics
// ============================================================================
// Walk PipboyMenu's display tree to find FallUI's addon holder and our addon.
// This tells us whether the addon SWF was loaded, and if so, what happened.

static void WalkDisplayObject(GFxValue* obj, int depth, int maxDepth)
{
	if (depth > maxDepth) return;

	// Get the name
	GFxValue nameVal;
	const char* name = "(unnamed)";
	if (obj->HasMember("name") && obj->GetMember("name", &nameVal) &&
		nameVal.GetType() == GFxValue::kType_String)
		name = nameVal.GetString();

	// Get numChildren if it's a container
	GFxValue numChildrenVal;
	int numChildren = -1;
	if (obj->HasMember("numChildren") && obj->GetMember("numChildren", &numChildrenVal))
		numChildren = (int)numChildrenVal.GetNumber();

	// Build indent (2 spaces per level)
	char indent[64] = {0};
	for (int i = 0; i < depth * 2 && i < 60; i++) indent[i] = ' ';

	if (numChildren >= 0)
		_MESSAGE("UnreadNotes: %s[%s] (%d children)", indent, name, numChildren);
	else
		_MESSAGE("UnreadNotes: %s[%s]", indent, name);

	// Recurse into children using Invoke("getChildAt", index)
	if (numChildren > 0 && depth < maxDepth)
	{
		for (int i = 0; i < numChildren && i < 30; i++)
		{
			GFxValue indexArg;
			indexArg.SetInt(i);
			GFxValue child;
			if (obj->Invoke("getChildAt", &child, &indexArg, 1) && child.IsObject())
			{
				WalkDisplayObject(&child, depth + 1, maxDepth);
			}
		}
	}
}

// ============================================================================
// Dimming — Walk visible renderers and set alpha based on read status
// ============================================================================
static bool g_dimmingInProgress = false;
// BSScrollingList (List_mc) has:
//   - entryList: Array of data objects (each has formID, filterFlag, text, etc.)
//   - numChildren: includes renderers as display children
//   - Each renderer child has an itemIndex property mapping to entryList index
//
// We walk the list's children, check each renderer's data entry, and dim
// read notes/holotapes to 50% alpha.

// SEH wrapper — __try/__except can't coexist with C++ destructors
// in the same function, so we wrap the actual work.
static void ApplyDimmingImpl();

static void ApplyDimmingSafe()
{
	__try { ApplyDimmingImpl(); }
	__except(EXCEPTION_EXECUTE_HANDLER)
	{ _MESSAGE("UnreadNotes: Dimming caught SEH exception"); }
}

class ApplyDimmingTask : public ITaskDelegate
{
public:
	int retriesLeft;

	ApplyDimmingTask(int retries = 0) : retriesLeft(retries) {}

	virtual void Run() override
	{
		g_dimmingInProgress = false;

		if (retriesLeft > 0)
			_MESSAGE("UnreadNotes: Dimming retry %d", retriesLeft);

		ApplyDimmingSafe();

		if (retriesLeft > 0 && !g_readNotes.empty() && g_taskInterface)
		{
			g_dimmingInProgress = true;
			g_taskInterface->AddUITask(new ApplyDimmingTask(retriesLeft - 1));
		}
	}
};

static const char* kEntryHolderPath =
	"root.Menu_mc.CurrentPage.List_mc.entryHolder_mc";
static const char* kEntryListPath =
	"root.Menu_mc.CurrentPage.List_mc.entryList";

// Try to register our Scaleform function as a FallUI injection handler.
// This would run our code inside FallUI's SetEntryText render cycle —
// exactly what the SWF patch did, but without modifying the SWF.
static void TryRegisterInjectionHandler()
{
	GFxMovieRoot* movieRoot = GetPipboyMovieRoot();
	if (!movieRoot) return;

	// Create our handler function
	GFxValue hookFn;
	movieRoot->CreateFunction(&hookFn, &g_setEntryTextHook);

	// Try multiple ways to call BSListMod.modAddInjectionHandler (static)
	const char* paths[] = {
		// Via movieRoot->Invoke with various path formats
		"M8r.Mod.BSListMod.modAddInjectionHandler",
		"M8r.Mod::BSListMod.modAddInjectionHandler",
		nullptr
	};

	GFxValue args[4];
	movieRoot->CreateString(&args[0], "generic");
	movieRoot->CreateString(&args[1], "SetEntryText");
	args[2] = hookFn;
	args[3].SetNull();

	for (int i = 0; paths[i]; i++)
	{
		GFxValue result;
		bool ok = movieRoot->Invoke(paths[i], &result, args, 4);
		_MESSAGE("UnreadNotes: Invoke(%s) = %d", paths[i], ok);
		if (ok) return;
	}

	// Try via List_mc.mod instance (even though it's static)
	GFxValue listMod;
	if (movieRoot->GetVariable(&listMod, "root.Menu_mc.CurrentPage.List_mc.mod") &&
		listMod.IsObject())
	{
		GFxValue result;
		bool ok = listMod.Invoke("modAddInjectionHandler", &result, args, 4);
		_MESSAGE("UnreadNotes: List_mc.mod.modAddInjectionHandler = %d", ok);
	}

	// Try getting BSListMod as a class reference
	GFxValue bsListModClass;
	if (movieRoot->GetVariable(&bsListModClass, "M8r.Mod.BSListMod") &&
		bsListModClass.IsObject())
	{
		GFxValue result;
		bool ok = bsListModClass.Invoke("modAddInjectionHandler", &result, args, 4);
		_MESSAGE("UnreadNotes: BSListMod.modAddInjectionHandler = %d", ok);

		// Also check: can we see the static modInjectionHandler dictionary?
		GFxValue injDict;
		if (bsListModClass.HasMember("modInjectionHandler") &&
			bsListModClass.GetMember("modInjectionHandler", &injDict))
		{
			_MESSAGE("UnreadNotes: modInjectionHandler type=%d", injDict.GetType());

			// Check if "generic" key exists
			GFxValue genericEntry;
			if (injDict.HasMember("generic") && injDict.GetMember("generic", &genericEntry))
			{
				_MESSAGE("UnreadNotes: modInjectionHandler['generic'] type=%d", genericEntry.GetType());

				GFxValue setEntryEntry;
				if (genericEntry.HasMember("SetEntryText") &&
					genericEntry.GetMember("SetEntryText", &setEntryEntry))
				{
					_MESSAGE("UnreadNotes: modInjectionHandler['generic']['SetEntryText'] type=%d",
						setEntryEntry.GetType());
					if (setEntryEntry.IsArray())
						_MESSAGE("UnreadNotes:   array size = %d", setEntryEntry.GetArraySize());
				}
				else
				{
					_MESSAGE("UnreadNotes: 'SetEntryText' NOT found in generic dict");
					// Try to create the structure manually
					GFxValue handlerArray;
					movieRoot->CreateArray(&handlerArray);

					GFxValue handlerObj;
					movieRoot->CreateObject(&handlerObj);
					handlerObj.SetMember("handler", &hookFn);
					GFxValue nullVal; nullVal.SetNull();
					handlerObj.SetMember("extraData", &nullVal);

					GFxValue pushArgs[1] = { handlerObj };
					handlerArray.Invoke("push", nullptr, pushArgs, 1);

					genericEntry.SetMember("SetEntryText", &handlerArray);
					g_hookRegistered = true;
					_MESSAGE("UnreadNotes: Manually added SetEntryText handler to generic dict");
				}
			}
			else
			{
				_MESSAGE("UnreadNotes: 'generic' NOT found — creating structure");
				// Build: modInjectionHandler["generic"]["SetEntryText"] = [{handler, extraData}]
				GFxValue genericDict;
				movieRoot->CreateObject(&genericDict);

				GFxValue handlerArray;
				movieRoot->CreateArray(&handlerArray);

				GFxValue handlerObj;
				movieRoot->CreateObject(&handlerObj);
				handlerObj.SetMember("handler", &hookFn);
				GFxValue nullVal; nullVal.SetNull();
				handlerObj.SetMember("extraData", &nullVal);

				GFxValue pushArgs[1] = { handlerObj };
				handlerArray.Invoke("push", nullptr, pushArgs, 1);

				genericDict.SetMember("SetEntryText", &handlerArray);
				injDict.SetMember("generic", &genericDict);
				g_hookRegistered = true;
				_MESSAGE("UnreadNotes: Manually built injection handler structure");
			}
		}
		else
		{
			_MESSAGE("UnreadNotes: modInjectionHandler NOT accessible (private static)");
		}
	}
}

static void ApplyDimmingImpl()
{
	GFxMovieRoot* movieRoot = GetPipboyMovieRoot();
	if (!movieRoot) return;

	GFxValue entryList;
	if (!movieRoot->GetVariable(&entryList, kEntryListPath) || !entryList.IsArray())
		return;  // Not on inventory page — silently skip
	UInt32 entryCount = entryList.GetArraySize();

	GFxValue entryHolder;
	if (!movieRoot->GetVariable(&entryHolder, kEntryHolderPath) || !entryHolder.IsObject())
		return;  // No entry holder — silently skip

	char pathBuf[256];
	snprintf(pathBuf, sizeof(pathBuf), "%s.numChildren", kEntryHolderPath);
	GFxValue numRendVal;
	if (!movieRoot->GetVariable(&numRendVal, pathBuf))
		return;
	int numRenderers = (int)numRendVal.GetNumber();

	// ========================================================================
	// EXPERIMENT: Try multiple visual approaches on entryList DATA entries.
	// Modifying the data (not renderers) should self-correct when FallUI
	// re-renders for a different subcategory, because FallUI reads from data.
	// ========================================================================

	int modified = 0, skipped = 0;

	// Walk the ENTIRE entryList (not just visible renderers).
	// This way, our changes persist in the data regardless of which
	// renderers are currently showing.
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
		{
			g_readNotes.insert(formID);
		}

		bool isRead = g_readNotes.count(formID) > 0;
		if (!isRead) continue;

		// Get current text
		GFxValue textVal;
		if (!dataEntry.HasMember("text") ||
			!dataEntry.GetMember("text", &textVal) ||
			textVal.GetType() != GFxValue::kType_String)
			continue;

		const char* text = textVal.GetString();

		// Skip if already marked (avoid double-applying)
		if (strstr(text, g_cfgDimColor) ||
			(g_cfgSuffix[0] && strstr(text, g_cfgSuffix)))
		{
			skipped++;
			continue;
		}

		// Build modified text:
		// 1. Find the [Tag] prefix boundary
		// 2. Wrap the item name (after tag) in HTML <font color> for dimming
		// 3. Append suffix if configured
		//
		// FallUI's Parser.parseItemName strips [Tag] prefixes via regex,
		// so we keep the tag intact and modify what comes after it.
		const char* tagEnd = text;
		if (text[0] == '[')
		{
			const char* bracket = strchr(text, ']');
			if (bracket)
				tagEnd = bracket + 1;
		}

		char buf[512];
		if (g_cfgBrightness < 1.0f)
		{
			// Wrap in HTML font colour for dimming.
			// FallUI detects '<' in text and switches to HTML rendering mode,
			// which bypasses its own textColor setting (line 170 BSEntryMod.as).
			snprintf(buf, sizeof(buf), "%.*s<font color='%s'>%s%s</font>",
				(int)(tagEnd - text), text,
				g_cfgDimColor, tagEnd, g_cfgSuffix);
		}
		else if (g_cfgSuffix[0])
		{
			// No dimming (brightness=100%), just suffix
			snprintf(buf, sizeof(buf), "%s%s", text, g_cfgSuffix);
		}
		else
		{
			continue;  // Nothing to modify
		}

		// === SORTING: modify textClean to push read items to bottom ===
		if (g_cfgSortReadToBottom)
		{
			GFxValue textCleanVal;
			if (dataEntry.HasMember("textClean") &&
				dataEntry.GetMember("textClean", &textCleanVal) &&
				textCleanVal.GetType() == GFxValue::kType_String)
			{
				const char* textClean = textCleanVal.GetString();
				if (textClean[0] != '~')
				{
					char newClean[512];
					snprintf(newClean, sizeof(newClean), "~%s", textClean);
					GFxValue newCleanVal;
					movieRoot->CreateString(&newCleanVal, newClean);
					dataEntry.SetMember("textClean", &newCleanVal);
				}
			}
		}

		GFxValue newTextVal;
		movieRoot->CreateString(&newTextVal, buf);
		dataEntry.SetMember("text", &newTextVal);

		modified++;

		_MESSAGE("UnreadNotes: Modified data[%u] formID=%08X: \"%s\"",
			idx, formID, buf);
	}

	// Try multiple approaches to force a visual refresh.
	// The core problem: UITask changes happen AFTER Scaleform's render
	// pass for the current frame, so modifications don't display.
	if (modified > 0)
	{
		_MESSAGE("UnreadNotes: Trying refresh approaches...");

		// Approach 1: InvalidateData via movieRoot->Invoke (full path)
		// This might go through a different code path than GFxValue::Invoke
		GFxValue invResult;
		bool ok1 = movieRoot->Invoke(
			"root.Menu_mc.CurrentPage.List_mc.InvalidateData",
			&invResult, nullptr, 0);
		_MESSAGE("UnreadNotes:   movieRoot->Invoke(InvalidateData) = %d", ok1);

		// Approach 2: Dispatch "change" event on the list
		GFxValue listMc;
		if (movieRoot->GetVariable(&listMc, "root.Menu_mc.CurrentPage.List_mc") &&
			listMc.IsObject())
		{
			// Create Event("change", true, true)
			GFxValue eventArgs[3];
			movieRoot->CreateString(&eventArgs[0], "change");
			eventArgs[1].SetBool(true);  // bubbles
			eventArgs[2].SetBool(true);  // cancelable
			GFxValue changeEvent;
			movieRoot->CreateObject(&changeEvent, "flash.events.Event",
				eventArgs, 3);
			GFxValue dispatchArgs[1] = { changeEvent };
			bool ok2 = listMc.Invoke("dispatchEvent", nullptr, dispatchArgs, 1);
			_MESSAGE("UnreadNotes:   dispatchEvent(change) = %d", ok2);

			// Approach 3: Dispatch "listUpdated" / "dataChange" events
			const char* eventNames[] = {
				"dataChange", "listUpdated", "scroll", nullptr
			};
			for (int i = 0; eventNames[i]; i++)
			{
				movieRoot->CreateString(&eventArgs[0], eventNames[i]);
				GFxValue evt;
				movieRoot->CreateObject(&evt, "flash.events.Event",
					eventArgs, 3);
				GFxValue args[1] = { evt };
				bool ok = listMc.Invoke("dispatchEvent", nullptr, args, 1);
				_MESSAGE("UnreadNotes:   dispatchEvent(%s) = %d", eventNames[i], ok);
			}

			// Approach 4: Call UpdateList via movieRoot->Invoke
			bool ok4 = movieRoot->Invoke(
				"root.Menu_mc.CurrentPage.List_mc.UpdateList",
				nullptr, nullptr, 0);
			_MESSAGE("UnreadNotes:   movieRoot->Invoke(UpdateList) = %d", ok4);
		}
	}

	if (g_markAllReadPending)
	{
		_MESSAGE("UnreadNotes: DEBUG — Marked all readable items as read (total: %u)",
			g_readNotes.size());
		g_markAllReadPending = false;
	}

	_MESSAGE("UnreadNotes: Applied — %d modified, %d skipped (already done), %u read notes tracked",
		modified, skipped, g_readNotes.size());
}

static void TryRegisterInjectionHandlerSafe()
{
	__try { TryRegisterInjectionHandler(); }
	__except(EXCEPTION_EXECUTE_HANDLER)
	{ _MESSAGE("UnreadNotes: Hook registration caught SEH exception"); }
}

class HookRegistrationTask : public ITaskDelegate
{
public:
	int retries;
	HookRegistrationTask(int r) : retries(r) {}
	virtual void Run() override
	{
		if (g_hookRegistered) return;
		TryRegisterInjectionHandlerSafe();
		if (!g_hookRegistered && retries > 0 && g_taskInterface)
			g_taskInterface->AddUITask(new HookRegistrationTask(retries - 1));
	}
};

static void QueueDimming(int retries = 5)
{
	if (g_taskInterface && !g_dimmingInProgress)
	{
		g_dimmingInProgress = true;
		g_taskInterface->AddUITask(new ApplyDimmingTask(retries));
	}
}


// ============================================================================
// Menu Event Handler
// ============================================================================
class MenuEventHandler : public BSTEventSink<MenuOpenCloseEvent>
{
public:
	virtual ~MenuEventHandler() {}

	virtual EventResult ReceiveEvent(MenuOpenCloseEvent* evn, void* dispatcher) override
	{
		const char* name = evn->menuName.c_str();

		// --- MarkAsRead: detect note/holotape activation ---
		if (evn->isOpen)
		{
			bool isBookMenu = (strcmp(name, "BookMenu") == 0);
			bool isTerminalMenu = (strcmp(name, "TerminalMenu") == 0);

			if (isBookMenu || isTerminalMenu)
			{
				GFxMovieRoot* movieRoot = GetPipboyMovieRoot();
				if (movieRoot)
				{
					UInt32 formID = 0;
					if (GetSelectedReadableItem(movieRoot, formID))
					{
						bool isNew = g_readNotes.insert(formID).second;
						if (isNew)
						{
							_MESSAGE("UnreadNotes: Marked FormID %08X as read (total: %u)",
								formID, g_readNotes.size());
						}
					}
				}
			}
		}

		if (evn->isOpen && strcmp(name, "PipboyMenu") == 0)
		{
			LoadConfig();  // Hot-reload config on each Pip-Boy open
			g_hookRegistered = false;  // Reset so we retry injection handler
			ScaleformSetEntryTextHook::s_loggedFirst = false;
			g_needsDisplayRefresh = !g_readNotes.empty();
			_MESSAGE("UnreadNotes: PipboyMenu opened — refresh=%d, queueing dimming",
				g_needsDisplayRefresh);
			QueueDimming(5);

			// TODO: FallUI injection handler registration — private static dict
			// is not accessible from C++. Need alternative approach.
		}

		// When returning from reading a note (BookMenu/TerminalMenu closes),
		// re-apply dimming so the just-read item gets dimmed immediately
		if (!evn->isOpen)
		{
			bool isBookMenu = (strcmp(name, "BookMenu") == 0);
			bool isTerminalMenu = (strcmp(name, "TerminalMenu") == 0);

			if (isBookMenu || isTerminalMenu)
			{
				_MESSAGE("UnreadNotes: %s closed — re-applying dimming", name);
				QueueDimming(2);  // Few retries — page is already stable
			}
		}

		// CursorMenu close indicates a Pip-Boy navigation completed.
		// Re-apply dimming to catch recycled renderers after category switches.
		// Guard (g_dimmingInProgress) prevents overlapping UI tasks.
		if (!evn->isOpen && strcmp(name, "CursorMenu") == 0)
		{
			_MESSAGE("UnreadNotes: CursorMenu closed — re-applying dimming");
			QueueDimming();
		}

		return kEvent_Continue;
	}
};

static MenuEventHandler g_menuEventHandler;


// ============================================================================
// F4SE Message Handler
// ============================================================================
void OnF4SEMessage(F4SEMessagingInterface::Message* msg)
{
	if (msg->type == F4SEMessagingInterface::kMessage_GameDataReady)
	{
		bool isReady = reinterpret_cast<bool>(msg->data);
		if (isReady)
		{
			if (*g_ui)
			{
				(*g_ui)->menuOpenCloseEventSource.AddEventSink(&g_menuEventHandler);
				_MESSAGE("UnreadNotes: Menu event handler registered");
			}
		}
	}
}


// ============================================================================
// AdvanceMovie Hook — runs our code INSIDE the per-frame menu update
// ============================================================================
// UITask changes happen AFTER Scaleform commits the frame, so they don't
// display until the next natural re-render. By hooking AdvanceMovie, our
// code runs DURING the frame update — changes are rendered immediately.

typedef void (*_AdvanceMovie_Original)(GameMenuBase*, float, void*);
_AdvanceMovie_Original AdvanceMovie_Original = nullptr;
RelocAddr<uintptr_t> AdvanceMovie_Addr(0x0210EED0);

void AdvanceMovie_Hook(GameMenuBase* menu, float unk0, void* unk1)
{
	// Call original first — lets FallUI do its normal rendering
	AdvanceMovie_Original(menu, unk0, unk1);

	// Only act when Pip-Boy is open and we have read notes to display
	if (g_readNotes.empty())
		return;

	// Check if this is the PipboyMenu by checking if it's open
	if (!menu->movie || !menu->movie->movieRoot)
		return;

	BSFixedString pipboyName("PipboyMenu");
	if (!(*g_ui)->IsMenuOpen(pipboyName))
		return;

	IMenu* pipMenu = (*g_ui)->GetMenu(pipboyName);
	if ((IMenu*)menu != pipMenu)
		return;

	// We're inside PipboyMenu's AdvanceMovie. Apply our data modifications
	// here — they'll be rendered in THIS frame.
	GFxMovieRoot* movieRoot = menu->movie->movieRoot;

	GFxValue entryList;
	if (!movieRoot->GetVariable(&entryList, kEntryListPath) || !entryList.IsArray())
		return;

	UInt32 entryCount = entryList.GetArraySize();
	if (entryCount == 0) return;

	// Quick check: find the first read item and see if it's already modified.
	// This runs every frame but is fast — one string check. Only if the
	// data is unmodified do we do the full entryList walk.
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

		// Found a read item — check if already modified
		GFxValue tv;
		if (de.HasMember("text") && de.GetMember("text", &tv) &&
			tv.GetType() == GFxValue::kType_String)
		{
			const char* t = tv.GetString();
			if (!strstr(t, g_cfgDimColor) &&
				!(g_cfgSuffix[0] && strstr(t, g_cfgSuffix)))
				needsModification = true;  // Unmodified text — need to apply
		}
	}

	if (!needsModification)
		return;

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
		if (g_readNotes.count(formID) == 0)
			continue;

		GFxValue textVal;
		if (!dataEntry.HasMember("text") ||
			!dataEntry.GetMember("text", &textVal) ||
			textVal.GetType() != GFxValue::kType_String)
			continue;

		const char* text = textVal.GetString();
		if (strstr(text, g_cfgDimColor) ||
			(g_cfgSuffix[0] && strstr(text, g_cfgSuffix)))
			continue;  // Already modified

		const char* tagEnd = text;
		if (text[0] == '[')
		{
			const char* bracket = strchr(text, ']');
			if (bracket) tagEnd = bracket + 1;
		}

		char buf[512];
		if (g_cfgBrightness < 1.0f)
		{
			snprintf(buf, sizeof(buf), "%.*s<font color='%s'>%s%s</font>",
				(int)(tagEnd - text), text, g_cfgDimColor, tagEnd, g_cfgSuffix);
		}
		else if (g_cfgSuffix[0])
		{
			snprintf(buf, sizeof(buf), "%s%s", text, g_cfgSuffix);
		}
		else continue;

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

	if (modified > 0)
	{
		// Call InvalidateData from INSIDE AdvanceMovie — this should
		// trigger a re-render in the same frame cycle.
		GFxValue listMc;
		if (movieRoot->GetVariable(&listMc, "root.Menu_mc.CurrentPage.List_mc") &&
			listMc.IsObject())
		{
			listMc.Invoke("InvalidateData", nullptr, nullptr, 0);
		}

		_MESSAGE("UnreadNotes: AdvanceMovie hook — modified %d entries + InvalidateData",
			modified);
	}
}

static void InstallAdvanceMovieHook()
{
	// Check the first bytes at the hook address to determine prologue size
	UInt8* funcStart = (UInt8*)AdvanceMovie_Addr.GetUIntPtr();
	_MESSAGE("UnreadNotes: AdvanceMovie at %p, first bytes: %02X %02X %02X %02X %02X %02X",
		funcStart, funcStart[0], funcStart[1], funcStart[2],
		funcStart[3], funcStart[4], funcStart[5]);

	struct AdvanceMovie_Code : Xbyak::CodeGenerator
	{
		AdvanceMovie_Code(void* buf, uintptr_t origAddr) : Xbyak::CodeGenerator(4096, buf)
		{
			Xbyak::Label retnLabel;

			// Reproduce the original function prologue (first 6 bytes)
			// We'll use Write6Branch which replaces 6 bytes
			// The typical prologue is: push rbp; sub rsp, XX or similar
			// We need to check the actual bytes and reproduce them
			UInt8* src = (UInt8*)origAddr;

			// Copy first 6 bytes as raw data
			for (int i = 0; i < 6; i++)
				db(src[i]);

			// Jump back to original function after the hooked bytes
			jmp(ptr[rip + retnLabel]);

			L(retnLabel);
			dq(origAddr + 6);
		}
	};

	void* codeBuf = g_localTrampoline.StartAlloc();
	AdvanceMovie_Code code(codeBuf, AdvanceMovie_Addr.GetUIntPtr());
	g_localTrampoline.EndAlloc(code.getCurr());

	AdvanceMovie_Original = (_AdvanceMovie_Original)codeBuf;

	g_branchTrampoline.Write6Branch(AdvanceMovie_Addr.GetUIntPtr(),
		(uintptr_t)AdvanceMovie_Hook);

	_MESSAGE("UnreadNotes: AdvanceMovie hook installed at %p", funcStart);
}


// ============================================================================
// Plugin Entry Points
// ============================================================================
extern "C"
{

__declspec(dllexport) bool F4SEPlugin_Query(const F4SEInterface* f4se, PluginInfo* info)
{
	gLog.OpenRelative(CSIDL_MYDOCUMENTS, "\\My Games\\Fallout4\\F4SE\\UnreadNotes.log");

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
	F4SEScaleformInterface* scaleform = (F4SEScaleformInterface*)f4se->QueryInterface(kInterface_Scaleform);
	if (!scaleform)
	{
		_FATALERROR("UnreadNotes: couldn't get Scaleform interface");
		return false;
	}
	scaleform->Register("UnreadNotes", ScaleformCallback);

	// --- Serialization ---
	F4SESerializationInterface* serialization = (F4SESerializationInterface*)f4se->QueryInterface(kInterface_Serialization);
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
	g_taskInterface = (F4SETaskInterface*)f4se->QueryInterface(kInterface_Task);
	if (!g_taskInterface)
	{
		_FATALERROR("UnreadNotes: couldn't get Task interface");
		return false;
	}

	// --- Messaging ---
	g_messaging = (F4SEMessagingInterface*)f4se->QueryInterface(kInterface_Messaging);
	if (!g_messaging)
	{
		_FATALERROR("UnreadNotes: couldn't get Messaging interface");
		return false;
	}
	g_messaging->RegisterListener(g_pluginHandle, "F4SE", OnF4SEMessage);

	// --- AdvanceMovie Hook ---
	// Allocate trampoline space and install the hook
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

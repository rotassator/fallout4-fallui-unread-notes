// UnreadNotes - F4SE Plugin
// Tracks read/unread status for notes and holotapes via the Pip-Boy.
// Works with FallUI — a patched Pipboy_InvPage.swf calls MarkAsRead()
// when items are activated and IsNoteRead() to dim read items.

#include "f4se_common/f4se_version.h"
#include "f4se/PluginAPI.h"
#include "common/IDebugLog.h"
#include "f4se/ScaleformCallbacks.h"

#include <shlobj.h>
#include <set>
#include <vector>
#include <algorithm>

static PluginHandle g_pluginHandle = kPluginHandle_Invalid;

// ============================================================================
// Read Tracking Data
// ============================================================================
static std::set<UInt32> g_readNotes;

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
// Called from FallUI's patched Pipboy_InvPage.swf via:
//   stage.getChildAt(0).f4se.plugins.UnreadNotes.<FunctionName>

class ScaleformGetVersion : public GFxFunctionHandler
{
public:
	virtual void Invoke(Args* args)
	{
		args->result->SetUInt(1);
	}
};

class ScaleformIsNoteRead : public GFxFunctionHandler
{
public:
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
	}
};

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

// SortByReadStatus(itemArray) -> void
// Called from FallUI's modSetItemList after the entry list is populated.
// Iterates the item array, finds notes/holotapes (filterFlag & 0x2280),
// checks read status, and does a stable partition so unread items come first.
//
// We do this in C++ rather than Flash because:
//   1. Loops in AVM2 P-code are painful to write by hand
//   2. We have direct access to g_readNotes (no per-item Scaleform call overhead)
//   3. std::stable_partition is cleaner than anything we'd write in P-code
class ScaleformSortByReadStatus : public GFxFunctionHandler
{
public:
	virtual void Invoke(Args* args)
	{
		if (args->numArgs < 1) return;

		GFxValue* arr = &args->args[0];
		UInt32 size = arr->GetArraySize();
		if (size == 0) return;

		// Tag each item with _readSort: 0 = unread note/holotape, 1 = everything else.
		// Tags are set for future sort integration with FallUI's column sort system.
		for (UInt32 i = 0; i < size; i++)
		{
			GFxValue element;
			if (!arr->GetElement(i, &element))
				continue;

			UInt32 sortVal = 1;  // default: not an unread note

			GFxValue filterFlagVal;
			if (element.HasMember("filterFlag") && element.GetMember("filterFlag", &filterFlagVal))
			{
				UInt32 filterFlag = filterFlagVal.GetUInt();

				if (filterFlag & 0x2280)  // note (0x80) or holotape (0x2000)
				{
					GFxValue formIDVal;
					if (element.HasMember("formID") && element.GetMember("formID", &formIDVal))
					{
						UInt32 formID = formIDVal.GetUInt();
						sortVal = (g_readNotes.count(formID) == 0) ? 0 : 1;
					}
				}
			}

			GFxValue sortGfx;
			sortGfx.SetUInt(sortVal);
			element.SetMember("_readSort", &sortGfx);
		}

		// Sorting is done on the Flash side — to be integrated with
		// FallUI's column sort system in a future update.
		// We just tag the items here; the P-code calls:
		//   param1.sortOn("_readSort", Array.NUMERIC)
	}
};


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
	RegisterFunction<ScaleformSortByReadStatus>(value, movieRoot, "SortByReadStatus");

	return true;
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
	_MESSAGE("UnreadNotes v1: loading");

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

	_MESSAGE("UnreadNotes v1: loaded successfully");

	return true;
}

}  // extern "C"

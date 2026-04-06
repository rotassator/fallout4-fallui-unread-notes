# FallUI - Unread Notes and Holotapes

An F4SE plugin and FallUI patch for Fallout 4 that tracks which notes and holotapes you've read through your Pip-Boy. Unread items appear bright and sorted to the top of the list, while read items are dimmed and sorted below.

## Features

- **Unread items sorted to top** of the inventory list (within each subcategory)
- **Read items dimmed** at 50% opacity for clear visual distinction
- **Instant re-sort** when reading a note - no need to close and reopen the Pip-Boy
- **Tracks all readable types**: notes, holotapes (audio and menu-based), game tapes, and misc notes (recipes, schematics, contracts)
- **Persists across saves** via F4SE cosave system
- **Survives mod load order changes** (FormIDs resolved on load)
- **Works with all FallUI themes and colour schemes** (alpha-based dimming is independent of colour settings)
- **Null-safe** - won't break FallUI if the plugin is missing, or vice versa

## How it works

The Pip-Boy tracks what's been read *through it*. If you listened to a holotape at a terminal before picking it up, the Pip-Boy wasn't involved - it'll show as unread until you open it from your inventory.

## Requirements

- Fallout 4 v1.10.163 (GOG/pre-next-gen)
- [F4SE](https://f4se.silverlock.org/) v0.6.23+
- [FallUI - Inventory](https://www.nexusmods.com/fallout4/mods/48758)

## Installation

Install via your mod manager (recommended) or copy the contents of `dist/` into your Fallout 4 `Data/` folder:

```
Data/
├── F4SE/
│   └── Plugins/
│       └── UnreadNotes.dll
└── Interface/
    ├── Pipboy_InvPage.swf
    └── PipboyMenu.swf
```

Both `.swf` files must win any file conflicts with FallUI's originals (load after FallUI in your mod manager).

## Compatibility

- **FallUI display options**: Works with all colour schemes, icon settings, and column configurations. The opacity-based dimming is applied independently of FallUI's styling.
- **FallUI sorting**: Integrates with FallUI's built-in sort system. Unread status is used as a secondary sort key, so column header sorting (by name, value, weight, etc.) continues to work normally.
- **Other F4SE plugins**: No known conflicts. The plugin uses a unique cosave ID (`UNrd`) and Scaleform namespace (`UnreadNotes`).

## Technical details

### Components

1. **F4SE Plugin** (`UnreadNotes.dll`) - Exposes Scaleform functions (`MarkAsRead`, `IsNoteRead`, `SortByReadStatus`) and persists read state in the F4SE cosave alongside each game save.

2. **FallUI Inventory Patch** (`Pipboy_InvPage.swf`) - Three P-code patches:
   - `SelectItemTried`: Calls `MarkAsRead(formID)` when activating a note/holotape, then updates the sort tag and refreshes the list
   - `SetEntryText` (InvListEntry): Dims read items at 50% alpha
   - `modSetItemList`: Calls `SortByReadStatus(itemArray)` to tag items with read status on each inventory update

3. **FallUI Sort Patch** (`PipboyMenu.swf`) - Adds `_readSort` as a sort key in `GameItemDataExtractor.modSortItemsArrayByExtraDataKey`, integrated into FallUI's existing text and icon sort orders.

### Tracked item types

| Type | filterFlag | Examples |
|------|-----------|----------|
| Notes | `0x80` | Quest notes, personal logs, letters |
| Holotapes | `0x2000` | Audio logs, menu holotapes, game tapes |
| Misc notes | `0x200` | Recipes, schematics, contracts |

## Building from source

### Prerequisites

- Visual Studio 2022 with "Desktop development with C++" workload
- CMake 3.20+
- F4SE v0.6.23 source code
- [JPEXS Free Flash Decompiler](https://github.com/jindrapetrik/jpexs-decompiler) (for SWF editing)

### Building the DLL

The F4SE source path is configured in `CMakeLists.txt`. Adjust if your F4SE source is in a different location.

```bash
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The DLL is automatically copied to `dist/F4SE/Plugins/` and the Vortex mod folder (if configured).

### Deploying SWFs

After editing SWFs with JPEXS, deploy to the Vortex mod folder:

```bash
cmake --build build --target deploy-swf
```

### SWF patching

The patched SWFs in `dist/Interface/` are modified using JPEXS P-code editing. See the commit history for the specific P-code changes applied to FallUI's original SWF files.

## License

[MIT](LICENSE)

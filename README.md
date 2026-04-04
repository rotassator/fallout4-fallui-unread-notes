# FallUI - Unread Notes

An F4SE plugin and FallUI patch for Fallout 4 that tracks which notes and holotapes you've read through your Pip-Boy. Read items appear dimmed in the inventory list so you can tell at a glance what's new.

## Features

- Tracks notes and holotapes when opened from the Pip-Boy inventory
- Read items displayed at reduced opacity (50%) in the misc inventory list
- Persists across saves via F4SE cosave system
- Survives mod load order changes (FormIDs resolved on load)
- Works with both mouse click and keyboard activation
- Null-safe — won't break FallUI if the plugin is missing, or vice versa

## Requirements

- Fallout 4 v1.10.163 (GOG/pre-next-gen)
- [F4SE](https://f4se.silverlock.org/) v0.6.23+
- [FallUI - Inventory](https://www.nexusmods.com/fallout4/mods/48758)

## Installation

Copy the contents of `dist/` into your Fallout 4 `Data/` folder, or install via your mod manager:

```
Data/
├── F4SE/
│   └── Plugins/
│       └── UnreadNotes.dll
└── Interface/
    └── Pipboy_InvPage.swf
```

The patched `Pipboy_InvPage.swf` must win any file conflicts with FallUI's original.

## How it works

The Pip-Boy tracks what's been read *through it*. If you listened to a holotape at a terminal before picking it up, the Pip-Boy wasn't involved and has no way of knowing — it'll show as unread until you open it from your inventory.

### Components

1. **F4SE Plugin** (`UnreadNotes.dll`) — Exposes Scaleform functions (`MarkAsRead`, `IsNoteRead`) and persists read state in the F4SE cosave alongside each game save.

2. **FallUI Patch** (`Pipboy_InvPage.swf`) — Calls `MarkAsRead(formID)` when you activate a note or holotape from the Pip-Boy, and dims read items by checking `IsNoteRead(formID)` when rendering the inventory list.

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

The DLL is automatically copied to `dist/F4SE/Plugins/`.

### Editing the SWF

The patched SWF at `dist/Interface/Pipboy_InvPage.swf` is edited with JPEXS using P-code modifications. See the commit history for the specific P-code changes applied to FallUI's original `Pipboy_InvPage.swf`.

## License

[MIT](LICENSE)

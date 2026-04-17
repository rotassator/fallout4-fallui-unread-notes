# FallUI - Unread Notes and Holotapes

An F4SE plugin for Fallout 4 that tracks which notes and holotapes you've read through your Pip-Boy. Read items are dimmed and labelled so you can tell at a glance what you've already seen.

## Features

- **Read items dimmed** — configurable opacity applied to the entire row (text, item counts, icons)
- **"(Read)" suffix** on read item names (configurable)
- **Tracks notes, text holotapes, and audio holotapes**
- **Works with all FallUI colour schemes** — alpha-based dimming, not colour replacement
- **Persists across saves** via F4SE cosave system
- **Survives mod load order changes** — FormIDs resolved on load
- **No modified SWFs** — pure DLL plugin, no FallUI files are replaced
- **Configurable** — INI settings with hot-reload (just close and reopen the Pip-Boy)

## How it works

The plugin tracks notes and holotapes read through the Pip-Boy inventory. Detection uses two mechanisms:

- **Notes and text holotapes** — caught via `MenuOpenCloseEvent` when `BookMenu` or `TerminalMenu` opens while the Pip-Boy is also open. The currently-selected Pip-Boy item is marked as read.
- **Audio holotapes** — caught by polling the `HolotapePlaying` flag on the Pip-Boy's Flash data object and edge-detecting the `false→true` transition. The currently-selected item at that moment is the one being played. This works even for tapes that play in the background without opening any other menu.

An AdvanceMovie hook runs inside the game's per-frame menu update to modify the inventory list data and apply alpha dimming to renderers — ensuring changes display immediately and persist across tab switches.

## Requirements

- Fallout 4 v1.10.163 (GOG/pre-next-gen)
- [F4SE](https://f4se.silverlock.org/) v0.6.23+
- [FallUI - Inventory](https://www.nexusmods.com/fallout4/mods/48758)

## Installation

Install via your mod manager (recommended) or copy `UnreadNotes.dll` into:

```
Data/F4SE/Plugins/UnreadNotes.dll
```

An `UnreadNotes.ini` config file is created automatically on first run.

## Configuration

Edit `Data/F4SE/Plugins/UnreadNotes.ini` (auto-created on first run). Changes take effect on the next Pip-Boy open — no game restart needed.

```ini
[Display]
; Brightness of read items (0-100). 100 = no change, 0 = invisible.
iReadBrightness=50

; Text appended to read item names. ASCII only, no < > characters.
sSuffix=" (Read)"

; Logging level. 0 = minimal, 1 = normal, 2 = debug (includes per-frame perf stats).
iLogLevel=1

[Debug]
; Set to 1 and open Pip-Boy to trigger. Auto-resets to 0 after use.
bResetAll=0
bMarkAllRead=0
```

Log file: `Documents\My Games\Fallout4\F4SE\UnreadNotes.log`

## Tracked item types

| Type | filterFlag | Examples |
|------|-----------|----------|
| Notes | `0x80` | Quest notes, personal logs, letters |
| Holotapes | `0x2000` | Audio logs, terminal holotapes |

## Compatibility

- **FallUI themes**: Works with all colour schemes — dimming is alpha-based.
- **FallUI sorting**: Does not modify FallUI's sort order. Items remain in their natural sort position.
- **Other F4SE plugins**: No known conflicts. Uses unique cosave ID (`UNrd`) and Scaleform namespace (`UnreadNotes`).

## Building from source

### Prerequisites

- Visual Studio 2022 with "Desktop development with C++" workload
- CMake 3.20+
- F4SE v0.6.23 source code (with pre-built libs)

### Build

The F4SE source path is configured in `CMakeLists.txt`.

```bash
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The DLL is copied to `dist/F4SE/Plugins/` and the Vortex mod folder (if configured in CMakeLists.txt).

## License

[MIT](LICENSE)

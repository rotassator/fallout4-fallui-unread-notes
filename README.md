# FallUI - Unread Notes and Holotapes

An F4SE plugin for Fallout 4 that tracks which notes and holotapes you've read through your Pip-Boy. Read items are dimmed and labelled so you can tell at a glance what you've already seen.

## Features

- **Read items dimmed** — configurable opacity applied to the entire row (text, item counts, icons)
- **"(Read)" suffix** on read item names (configurable)
- **Toggle key** — configurable keypress to mark any readable Pip-Boy item as read or unread (notes, holotapes, magazines, game cartridges, misc items)
- **Mark key** — separate keypress to flag an item as "marked": stays bright, gets a distinct suffix, and is skipped by auto-mark-as-read. Handy for config holotapes (SKK etc.) and radiant notes with shared FormIDs.
- **Tracks notes, text holotapes, and audio holotapes**
- **Magazines included** — read through the Pip-Boy, they're suffixed and dimmed like notes
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

## Item states

Each tracked item is always in exactly one of three states:

| State | Visual | Set by |
|-------|--------|--------|
| Unread | Full brightness, no suffix | Default |
| Read | Dimmed, `" (Read)"` suffix | Auto (BookMenu / TerminalMenu / HolotapePlaying) or toggle key |
| Marked | Full brightness, `" (*)"` suffix | Mark key only. Auto-mark is skipped for marked items. |

The toggle key and mark key are mutually exclusive: marking a read item clears the read state, and toggling a marked item clears the marked state.

## Requirements

**Required:**

- Fallout 4 — any of:
  - OG 1.10.163 (pre-next-gen)
  - NG 1.10.984 (next-gen)
  - AE 1.11.x (anniversary edition)
- [F4SE](https://f4se.silverlock.org/) matching your runtime (v0.6.23+ for OG, v0.7.x for NG/AE)
- [Address Library for F4SE](https://www.nexusmods.com/fallout4/mods/47327) — with the `.bin` file matching your specific game version

**Recommended:**

- [FallUI - Inventory](https://www.nexusmods.com/fallout4/mods/48758) — needed for the dimming effect. Without FallUI, the `(Read)` suffix and toggle/mark keys still work, but read items aren't visually dimmed.

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

; Text appended to MARKED item names (bright, excluded from auto-mark-as-read).
sMarkSuffix=" (*)"

; Logging level. 0 = minimal, 1 = normal, 2 = debug (includes per-frame perf stats).
iLogLevel=1

[Input]

; Toggle read/unread on the selected Pip-Boy item with a keypress.
; Value is the decimal code from the Fallout CK wiki's scan-code table.
; Commented out by default so no key is claimed until you opt in.
; Reference: https://falloutck.uesp.net/wiki/DirectX_Scan_Codes
; Suggested unused keys: 189 ("-"), 187 ("="), 220 ("\")
;iToggleKey=189

; Mark/unmark the selected item. Marked items stay bright, get sMarkSuffix,
; and are skipped by auto-mark-as-read. Pick a different key from iToggleKey.
;iMarkKey=187

[Debug]
; Set to 1 and open Pip-Boy to trigger. Auto-resets to 0 after use.
bResetAll=0
bMarkAllRead=0
```

Log file: `Documents\My Games\Fallout4\F4SE\UnreadNotes.log`

## Tracked item types

| Type        | filterFlag | Examples                                         |
|-------------|-----------|---------------------------------------------------|
| Notes       | `0x80`    | Quest notes, personal logs, letters, magazines   |
| Holotapes   | `0x2000`  | Audio logs, terminal holotapes, game cartridges  |
| Misc items* | `0x200`   | Recipes, schematics, contracts (Shelley's etc.)  |

*Misc items and game cartridges aren't auto-marked — they don't open the book menu. Use the toggle key.

## Compatibility

- **FallUI themes**: Works with all colour schemes — dimming is alpha-based.
- **FallUI sorting**: Does not modify FallUI's sort order. Items remain in their natural sort position.
- **Other F4SE plugins**: No known conflicts. Uses unique cosave ID (`UNrd`) and Scaleform namespace (`UnreadNotes`).

## Building from source

### Prerequisites

- Visual Studio 2022 with "Desktop development with C++" workload
- [xmake](https://xmake.io/) 2.8+

### Build

Clone with submodules, configure, and build:

```bash
git clone --recurse-submodules https://github.com/rotassator/fallout4-fallui-unread-notes
cd fallout4-fallui-unread-notes
xmake f -p windows -a x64 -m releasedbg
xmake build
```

Pass `-p windows` explicitly — without it, xmake may auto-detect MinGW from Git's bundled toolchain and the build will fail at the spdlog install step.

The DLL is built to `build/windows/x64/releasedbg/UnreadNotes.dll` and auto-deployed to `dist/F4SE/Plugins/`. Optional local deploy targets (Vortex, MO2) in `xmake.lua` trigger if those paths exist.

### Submodule note

`lib/commonlibf4` currently points at a fork branch (rotassator/commonlibf4) pending [upstream PR #6](https://github.com/Dear-Modding-FO4/commonlibf4/pull/6), which adds a missing OG Scaleform `Value::GetMember` ID. The submodule will be bumped back to upstream Dear-Modding-FO4/commonlibf4 once the PR merges.

## License

[MIT](LICENSE)

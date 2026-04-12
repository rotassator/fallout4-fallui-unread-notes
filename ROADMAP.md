# UnreadNotes — Roadmap & Ideas

## Current State (v0.1 — Working Prototype)
- Read tracking via MenuOpenCloseEvent (BookMenu/TerminalMenu)
- Cosave persistence across saves
- HTML colour dimming on item text
- Configurable "(Read)" suffix
- Sort read items to bottom of subcategory
- INI config with hot-reload on Pip-Boy open
- Debug commands (bResetAll, bMarkAllRead)
- AdvanceMovie hook for reliable display refresh

## Performance
- [ ] Performance review — profile the AdvanceMovie hook. The quick-check walks entryList every frame looking for the first read item. With large inventories (370+ items) this could add up. Consider caching the index of a known read item for O(1) check instead of O(n) scan.
- [ ] Investigate whether string operations (strstr on text) in the quick-check are a bottleneck — could use a simpler flag property on the data entry instead.
- [ ] bMarkAllRead with 100+ items caused fan spin — investigate whether this is the modification loop or InvalidateData being heavy. Consider batching or throttling.
- [ ] Long-term stability: check for memory leaks or accumulating state. GFxValue CreateString calls in the modification loop — do these get garbage collected? Does the Parser.parseItemCache grow unbounded with our modified text keys?
- [ ] Measure actual frame time impact of the hook when idle (quick-check only) vs active (full modification walk).

## Bugs / Polish
- [ ] Item count column (e.g. "13", "100") not dimmed — FallUI renders these in separate TextFields
- [ ] Performance: quick-check walks entire entryList every frame — cache known read item index for O(1) check
- [ ] Performance: bMarkAllRead with 100+ items causes fan spin — throttle or batch modifications
- [ ] Code cleanup: remove dead code from failed approaches (old renderer alpha, FallUI addon, injection handler attempts, etc.)
- [ ] Suffix character restrictions: `< >` break HTML, `!` may cause issues — validate/sanitise in config loading

## Features — Near Term
- [ ] Configurable logging levels (INI: `iLogLevel=0-2`). 0=minimal (errors + startup), 1=normal (modifications, config changes, events), 2=debug (per-item details, hook firing, retry counts). Helps users include useful info in bug reports without flooding the log during normal play.
- [ ] **HIGH PRIORITY**: Renderer alpha dimming via AdvanceMovie hook — solves TWO problems at once: correct colour for all FallUI themes AND item count dimming. Walk renderers in the hook, check itemIndex → entryList data → read status, set renderer alpha. Recycling problem solved by correcting every frame. Would replace HTML colour dimming (keep suffix + sorting as text-based). Needs performance comparison vs current quick-check approach.
- [ ] MCM integration (replace or supplement INI config)
- [ ] Option to use a prefix instead of/as well as suffix (e.g. prepend a marker character)
- [ ] Proper colour dimming — read FallUI's colour from Data/MCM/Settings/FallUI.ini (iPipboyColorize + custom RGB) and multiply by iReadBrightness. Currently uses % of white which only works properly with PipboyFX filter mode. Also add `sCustomDimColor=` INI override (default empty) — if set to a valid hex like `#007F00`, uses that directly instead of computing from FallUI's colour. Gives users full manual control.
- [ ] Games category (Grognak, Pipfall, etc.) — check if they have a distinguishable filterFlag
- [ ] Misc notes (filterFlag 0x200 — recipes, schematics, contracts like Shelley's contract) — these appear in the Notes category by default but were excluded because they don't open BookMenu. Would need a different detection mechanism for "read" (maybe track when they're selected/viewed in the list?), or let users manually mark them, or just always show as "unread"

## Features — Medium Term
- [ ] Track reads from non-inventory context — currently MarkAsRead only fires when BookMenu/TerminalMenu opens while PipboyMenu is also open (reading from Pip-Boy). Investigate detecting when player reads a note/holotape picked up in the world (BookMenu/TerminalMenu without PipboyMenu open). Would need to identify the formID from the BookMenu/TerminalMenu itself or from the game engine's "currently viewed item" rather than from Pip-Boy's selectedEntry. If implemented, this would REPLACE the current PipboyMenu-dependent approach — one unified code path for all reads.

## Features — Future
- [ ] Custom icon for read/unread items (would need FallUI ExtraIcon integration or similar)
- [ ] Per-item "mark as unread" (via Pip-Boy interaction — maybe a hotkey while hovering?)
- [ ] Sorting options: alphabetical within read/unread groups, or by read order (most recently read first/last)
- [ ] Stats: "X of Y notes read" display somewhere in the Pip-Boy
- [ ] Optional notification when a new note/holotape is picked up ("New note added")
- [ ] Support for non-FallUI setups (vanilla Pip-Boy UI) — would need different text modification approach

## Technical Debt
- [ ] Remove old ScaleformSetEntryTextHook class
- [ ] Remove WalkDisplayObject diagnostic function
- [ ] Remove TryRegisterInjectionHandler and related dead code
- [ ] Remove unused Scaleform functions (SortByReadStatus)
- [ ] Remove UITask-based refresh code (event dispatching, etc.) — AdvanceMovie hook handles everything
- [ ] Consider whether ApplyDimmingImpl / UITask path is still needed or if AdvanceMovie hook replaces it entirely
- [ ] Review SEH exception handlers — still needed or can some be removed now?
- [ ] Proper git history: squash/clean experimental commits before release

## Publishing
- [ ] NexusMods page — full rewrite of description for v2 (pure C++ approach, no SWF patches)
- [ ] New screenshots showing dimming, "(Read)" suffix, sorting, config options
- [ ] Remove original mod file from NexusMods (old SWF-patching version)
- [ ] Review permissions — can be more permissive now (no FallUI SWFs included, just our own DLL)
- [ ] Thanks/credits section — FallUI/M8r (addon system research, decompiled source reference), F4SE team, any other references
- [ ] Requires F4SE (v0.6.23) — document this dependency
- [ ] Requires FallUI Inventory — document this (we rely on FallUI's HTML text mode + sort system)
- [ ] No ESP/ESM required — DLL-only mod
- [ ] Release archive: just UnreadNotes.dll (INI auto-created on first run)
- [ ] Changelog / versioning
- [ ] Document INI settings with examples in the mod description
- [ ] Compatibility notes: GOG and Steam versions, other Pip-Boy mods

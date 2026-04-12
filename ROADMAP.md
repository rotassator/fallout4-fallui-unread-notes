# UnreadNotes — Roadmap & Ideas

## Current State (v0.2 — Working)
- Read tracking via MenuOpenCloseEvent (BookMenu/TerminalMenu)
- Cosave persistence across saves
- Renderer alpha dimming (whole row: text, counts, icons)
- Configurable "(Read)" suffix
- Sort read items to bottom of subcategory
- AdvanceMovie hook for per-frame display refresh
- INI config with hot-reload on Pip-Boy open
- Debug commands (bResetAll, bMarkAllRead)
- Per-frame performance profiling (~180us avg, ~1% frame budget)

## Performance
- [ ] Optimise quick-check — currently walks entryList every frame looking for the first read item (O(n)). Cache the index of a known read item for O(1) check.
- [ ] Investigate whether a simple flag property on data entries would be faster than strstr on text for the quick-check.
- [ ] bMarkAllRead with 100+ items caused fan spin — investigate whether this is the modification loop or InvalidateData being heavy. Consider batching or throttling.
- [ ] Long-term stability: check for memory leaks. GFxValue CreateString calls — do these get GC'd? Does FallUI's Parser.parseItemCache grow unbounded with our modified text keys?
- [ ] Profile the renderer alpha walk separately from the text modification walk.

## Bugs / Polish
- [x] ~~Item count column not dimmed~~ — RESOLVED. Renderer alpha dims entire row.
- [x] ~~Code cleanup: remove dead code~~ — DONE. Removed 720 lines of experimental approaches.
- [x] ~~HTML colour doesn't match FallUI themes~~ — RESOLVED. Alpha dimming is colour-agnostic.
- [x] ~~Suffix character restrictions~~ — DONE. `< >` stripped on load with log warning.

## Features — Near Term
- [ ] Configurable logging levels (INI: `iLogLevel=0-2`). 0=minimal (errors + startup), 1=normal (modifications, config changes, events), 2=debug (per-item details, perf stats, hook firing). Gate perf logging behind level 2.
- [ ] MCM integration (replace or supplement INI config)
- [ ] Option to use a prefix instead of/as well as suffix (e.g. prepend a marker character)
- [ ] Games category (Grognak, Pipfall, etc.) — check if they have a distinguishable filterFlag
- [ ] Misc notes (filterFlag 0x200 — recipes, schematics, contracts like Shelley's contract) — appear in Notes category but don't open BookMenu. Would need a different "read" trigger (selection tracking?), or let users manually mark them.

## Features — Medium Term
- [ ] Track reads from non-inventory context — currently MarkAsRead only fires when BookMenu/TerminalMenu opens while PipboyMenu is also open. Investigate getting the formID from BookMenu/TerminalMenu directly (game engine's "currently viewed item"), which would cover both Pip-Boy reads AND world reads. Would REPLACE the current PipboyMenu-dependent approach — one unified code path.

## Features — Future
- [ ] Custom icon for read/unread items (would need FallUI ExtraIcon integration or similar)
- [ ] Per-item "mark as unread" (via Pip-Boy interaction — maybe a hotkey while hovering?)
- [ ] Sorting options: alphabetical within read/unread groups, or by read order (most recently read first/last)
- [ ] Stats: "X of Y notes read" display somewhere in the Pip-Boy
- [ ] Optional notification when a new note/holotape is picked up ("New note added")
- [ ] Support for non-FallUI setups (vanilla Pip-Boy UI)

## Technical Debt
- [x] ~~Review SEH exception handlers~~ — RESOLVED. All removed during cleanup. Hook code is stable. If crashes are reported, can add SEH wrapper around the hook body.
- [ ] Proper git history: squash/clean experimental commits before merging to develop

## Publishing
- [ ] NexusMods page — full rewrite of description for v2 (pure C++ approach, no SWF patches)
- [ ] New screenshots showing dimming, "(Read)" suffix, sorting, config options
- [ ] Remove original mod file from NexusMods (old SWF-patching version)
- [ ] Review permissions — more permissive now (no FallUI SWFs, just our DLL)
- [ ] Thanks/credits section — FallUI/M8r (decompiled source reference), F4SE team
- [ ] Requires F4SE (v0.6.23) — document dependency
- [ ] Requires FallUI Inventory — document dependency (we rely on FallUI's sort system + text parsing)
- [ ] No ESP/ESM required — DLL-only mod
- [ ] Release archive: just UnreadNotes.dll (INI auto-created on first run)
- [ ] Changelog / versioning
- [ ] Document INI settings with examples in the mod description
- [ ] Compatibility notes: GOG and Steam versions, other Pip-Boy mods

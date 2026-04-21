# UnreadNotes — Roadmap & Ideas

## Current State (v1.0 — Publish-ready)
- Read tracking for notes and text holotapes via MenuOpenCloseEvent (BookMenu/TerminalMenu)
- Read tracking for audio holotapes via `DataObj.HolotapePlaying` edge detection
- Cosave persistence across saves
- Renderer alpha dimming (whole row: text, counts, icons)
- Configurable "(Read)" suffix
- AdvanceMovie hook for per-frame display refresh
- INI config with hot-reload on Pip-Boy open
- Debug commands (bResetAll, bMarkAllRead)
- Configurable log levels (0=minimal, 1=normal, 2=debug)
- Per-frame performance profiling at log level 2 (~180us avg, ~1% frame budget)

**Sort-to-bottom parked** — see `feature/sort-to-bottom-wip` branch. InvalidateData calls during FallUI's sort disturb selectedEntry for audio holotapes. Needs a different approach (possibly hooking FallUI's sort at a lower level).

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

### From v1.0 user feedback (Nexus)
- [x] ~~**Read/unread toggle on keypress**~~ — DONE. Configurable scan code under `[Input]`, commented out by default. FallUI's menu dispatch uses Windows VK codes; values match UESP's table directly (no DIK conversion needed). Widened the visual/markable filter to include misc items (0x200) so anything toggled gets the suffix and dim.
- [x] ~~**Per-FormID ignore list** + **Bookmark/highlight tag**~~ — DONE, merged into a single "mark" feature. Configurable `iMarkKey` flips items between unmarked and marked; marked items stay bright, get `sMarkSuffix` (default `" (*)"`), are excluded from auto-mark-as-read. Mutually exclusive with the read state (marking a read item clears read, toggling a marked item clears mark). Persisted in a new `MkNt` cosave record alongside `RdNt`. Covers both the radiant-FormID-collision use case and the bookmark-for-later use case with one mechanism.

- [x] ~~Configurable logging levels~~ — DONE. iLogLevel=0-2. Perf stats gated behind level 2.
- [x] ~~Audio holotape detection~~ — DONE. Polls `root.Menu_mc.DataObj.HolotapePlaying` in AdvanceMovie_Hook and edge-detects the false→true transition. The tape-loading animation briefly drops the flag between plays, so seamless swaps (new tape without explicit stop) produce detectable cycles. First-sample suppression prevents spurious marks when reopening the Pipboy mid-playback.
- [ ] **Unified read-tracking for world reads** — BookMenu/TerminalMenu detection only fires while PipboyMenu is also open. Reading a note/holotape from the world (on a desk, in a terminal) doesn't mark it. Investigate getting the formID from BookMenu/TerminalMenu directly rather than via PipboyMenu selection lookup — would cover both contexts with one code path.
- [ ] Option to use a prefix instead of/as well as suffix (e.g. prepend a marker character)
- [x] ~~Games category~~ — PARTIAL. Confirmed they don't trigger `HolotapePlaying` or BookMenu. Manually markable via the toggle key; auto-detection would need a hook on the minigame-launch menu (not yet investigated).
- [x] ~~Misc notes (filterFlag 0x200 — recipes, schematics, contracts like Shelley's contract)~~ — PARTIAL. Manually markable via the toggle key (included in `kFilterMask_MarkableItems`); auto-detection still not possible since they don't open BookMenu.

## Features — Ideas (no commitment)
- [ ] MCM integration (replace or supplement INI config) — INI works fine, MCM is nice-to-have for the subset of users who prefer it. Deferred.
- [ ] Quest-marker cube on notes tied to active quests (FO76-style) — user request from Nexus v1.1 comments. Visual side is easy (inject coloured symbol into entry text). Blocker is the detection side: FO4 quest objectives reference aliases rather than specific item FormIDs, so reliably mapping "this objective wants you to read that exact note" would be patchy. Half-working would be worse than nothing.
- [ ] **"(Read)" tag on world activation prompt** — user request (Nexus v1.2 comments). Show read/marked state at the crosshair pickup prompt, before the item reaches the Pip-Boy. Detection is straightforward (crosshair ref → base form → `g_readNotes` lookup). Display requires HUDMenu text integration — either a hook exposed by FallUI - HUD, or a game-side hook into HUDMenu's activate-text routine. No SWF patches (per the hard rule). Worth a FallUI HUD decompile recon before committing.

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
Audio holotape detection (the last functional blocker) is resolved.
Remaining items are mostly documentation and release packaging.

- [ ] NexusMods page — full rewrite of description for v2 (pure C++ approach, no SWF patches)
- [ ] New screenshots showing dimming, "(Read)" suffix, config options
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

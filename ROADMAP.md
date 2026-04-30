# UnreadNotes — Roadmap & Ideas

## Current State (v1.3.0 — Released)
- **Multi-runtime support**: single DLL works on OG (1.10.163), NG (1.10.984), and AE (1.11.x), auto-detected at load. Built on CommonLibF4 + Address Library.
- Read tracking for notes and text holotapes via MenuOpenCloseEvent (BookMenu/TerminalMenu)
- Read tracking for audio holotapes via `DataObj.HolotapePlaying` edge detection
- Toggle key for manual read/unread on any item; mark key for "stay bright, skip auto-mark"
- Cosave persistence across saves (backwards-compatible with 1.2.1 cosaves)
- Renderer alpha dimming (whole row: text, counts, icons) — requires FallUI; suffix/toggle/mark function on vanilla Pip-Boy without it
- Configurable "(Read)" and "(*)" mark suffixes
- PipboyMenu vtable swap on AdvanceMovie slot 0x04 for per-frame display refresh
- INI config with hot-reload on Pip-Boy open
- Debug commands (bResetAll, bMarkAllRead)
- Configurable log levels (0=minimal, 1=normal, 2=debug)
- Startup runtime log line for diagnostic clarity in bug reports
- Per-frame performance profiling at log level 2 (~180us avg, ~1% frame budget — measured pre-CommonLibF4 port; should be similar post-port)

**Sort-to-bottom parked** — see `feature/sort-to-bottom-wip` branch and its `SORT_REVIVAL_NOTES.md` for two concrete revival ideas (FIS + title-mod synergy with snapshot-at-open mitigation; full `modSortingCallable` override). Underlying selection-tracking issue is in FallUI itself — independently reproducible in the cooking craft menu without UnreadNotes loaded — so any revival has to dodge the bug rather than fix it.

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

### Next release (v1.4.0): MCM migration

Post-recon design. MCM uses a two-layer INI scheme — defaults at `Data/MCM/Config/<Mod>/settings.ini` (shipped, refreshed each release), user overrides at `Data/MCM/Settings/<Mod>.ini` (MCM-written or hand-edited). This collapses the originally-planned "MCM integration" + "INI auto-merge" into a single architectural change: the two-layer scheme inherently solves the new-keys-don't-appear-in-existing-INI problem because defaults live in a separately-shipped file that gets refreshed each update without touching user values.

**Architecture.** DLL reads with a deterministic fallback chain:
1. `Data/MCM/Settings/UnreadNotes.ini` — user overrides (MCM-managed or hand-edited)
2. `Data/F4SE/Plugins/UnreadNotes.ini` — legacy / non-MCM users
3. `Data/MCM/Config/UnreadNotes/settings.ini` — defaults (shipped)
4. Hardcoded fallback constants

The DLL is file-based and indifferent to MCM's presence; MCM detection only matters for the one-time migration trigger.

**Migration + tombstone.** On first v1.4.0 load, if `Data/MCM/Config/MCM/` exists (MCM detected) AND `Data/MCM/Settings/UnreadNotes.ini` does not exist AND `Data/F4SE/Plugins/UnreadNotes.ini` exists with user values, copy values to the MCM Settings path. **Prepend** a state-agnostic tombstone to the legacy INI explaining the new location and shadow precedence — do **not** strip original values. Preserves downgrade safety (v1.3.0 still reads the legacy file), survives MCM uninstall (DLL reads MCM Settings INI regardless of MCM-the-mod's state), and handles late-MCM-install (trigger is idempotent — runs once when conditions are first met).

**Deliverables (implementation order):**

- [ ] Author shipped defaults INI at `Data/MCM/Config/UnreadNotes/settings.ini`. Content moved verbatim from the current `CreateDefaultConfig` string, with a header documenting the two-layer scheme for non-MCM users (mirroring FallUI's defaults-INI header pattern).
- [ ] Update `LoadConfig` (`src/main.cpp:182+`) to use the fallback chain. Remove `CreateDefaultConfig` since defaults now ship as a real file.
- [ ] Implement migration trigger + tombstone prepend. Idempotent — guard with the three conditions above.
- [ ] Author `Data/MCM/Config/UnreadNotes/config.json`. Menu mirrors the current INI sections (`Display`, `Input`, `Debug`). `pluginRequirements: []` per the Upscaling reference. Verify text-input widget support (for `sSuffix`/`sMarkSuffix`) and key-bind widget support (for `iToggleKey`/`iMarkKey`) during this step; document workarounds if unsupported.
- [ ] Update `xmake.lua` to deploy the new files into the mod archive's `Data/MCM/Config/UnreadNotes/` path.
- [ ] Verification matrix — walk the five user cases (new + MCM, new no-MCM, existing + MCM, existing no-MCM, MCM-disabled post-migration). Each must read settings correctly per the fallback chain.
- [ ] Documentation: README, Nexus description, CHANGELOG. Frame as "config UX, no gameplay changes."

**Out of scope.** Any gameplay/feature changes. Clean blast radius for testing — any regression is by definition in this migration, not bundled feature work. v1.5.0+ feature releases inherit MCM coverage by adding entries alongside any new INI keys.

### From v1.0 user feedback (Nexus)
- [x] ~~**Read/unread toggle on keypress**~~ — DONE. Configurable scan code under `[Input]`, commented out by default. FallUI's menu dispatch uses Windows VK codes; values match UESP's table directly (no DIK conversion needed). Widened the visual/markable filter to include misc items (0x200) so anything toggled gets the suffix and dim.
- [x] ~~**Per-FormID ignore list** + **Bookmark/highlight tag**~~ — DONE, merged into a single "mark" feature. Configurable `iMarkKey` flips items between unmarked and marked; marked items stay bright, get `sMarkSuffix` (default `" (*)"`), are excluded from auto-mark-as-read. Mutually exclusive with the read state (marking a read item clears read, toggling a marked item clears mark). Persisted in a new `MkNt` cosave record alongside `RdNt`. Covers both the radiant-FormID-collision use case and the bookmark-for-later use case with one mechanism.

- [x] ~~Configurable logging levels~~ — DONE. iLogLevel=0-2. Perf stats gated behind level 2.
- [x] ~~Audio holotape detection~~ — DONE. Polls `root.Menu_mc.DataObj.HolotapePlaying` in AdvanceMovie_Hook and edge-detects the false→true transition. The tape-loading animation briefly drops the flag between plays, so seamless swaps (new tape without explicit stop) produce detectable cycles. First-sample suppression prevents spurious marks when reopening the Pipboy mid-playback.
- [ ] **Unified read-tracking for world reads** — BookMenu/TerminalMenu detection only fires while PipboyMenu is also open. Reading a note/holotape from the world (on a desk, in a terminal) doesn't mark it. Investigate getting the formID from BookMenu/TerminalMenu directly rather than via PipboyMenu selection lookup — would cover both contexts with one code path. Now substantially easier post-1.3.0: any new menu-event hook gets OG/NG/AE coverage for free via Address Library.
- [ ] **Global title-modification hook** — broader reframing of the original "(Read) tag at world activation prompt" idea (Nexus v1.2 comments). Hook `TESFullName::GetFullName` / `InventoryEntryData::GetDisplayName` to inject the suffix at the canonical name source rather than per-frame in the renderer. The suffix then appears everywhere the engine reads the display name: world activation prompt, container/companion transfer UIs, favorites menu, vanilla Pip-Boy — and the existing per-frame renderer suffix injection becomes redundant (simplification). Pure live decoration: zero save state, removing the plugin fully reverts. Caveats: must drop the renderer suffix injection to avoid double-suffix; FallUI's alphabetical sort would group read items at the bottom as a side-effect (could be a feature, could surprise users); needs early-out by form type for perf since `GetFullName` is hot; gated by a new `bGlobalNameSuffix` INI flag (default on). Vtable swap pattern from `PipboyMenu::AdvanceMovie` is reusable; Address Library covers `RE::VTABLE::TESFullName` across all three runtimes.
- [x] ~~**Next-gen / AE compatibility**~~ — DONE in v1.3.0. Took the single-DLL CommonLibF4 + Address Library route rather than the dual-DLL CMake approach originally sketched here. AdvanceMovie hook reworked from xbyak branch trampoline at hardcoded `0x0210EED0` to a vtable swap on `RE::VTABLE::PipboyMenu[0]` slot 0x04 (covered by Address Library on all three runtimes). OG and AE confirmed in-game; NG community-tested.
- [ ] **Prefix option** (alongside or instead of suffix). Two sort-interaction implications to think through before implementation, since FallUI sorts on a separate `textClean` field derived from the displayed name with bracket-tag stripping:
  - *Plain-character prefix* (`* `, `> `) ends up in `textClean` → read items cluster together at the prefix's collation position within each category. Could be desired as visual grouping, but it's a behaviour change to document rather than an accidental side-effect.
  - *Bracket-tag prefix* (`[Read]`) opens the FIS sort-to-bottom synergy noted on the parked branch — if FIS is configured with the tag, `tagSortBefore` handles position and the bracket gets stripped from `textClean`. Without FIS recognising the tag, behaviour depends on whether FallUI's parser strips unknown brackets — needs a logging-build recon session.
  - Suffix-only mode (current default) has no perceptible sort impact within a category.
- [x] ~~Games category~~ — PARTIAL. Confirmed they don't trigger `HolotapePlaying` or BookMenu. Manually markable via the toggle key; auto-detection would need a hook on the minigame-launch menu (not yet investigated).
- [x] ~~Misc notes (filterFlag 0x200 — recipes, schematics, contracts like Shelley's contract)~~ — PARTIAL. Manually markable via the toggle key (included in `kFilterMask_MarkableItems`); auto-detection still not possible since they don't open BookMenu.

## Features — Ideas (no commitment)
- [ ] Quest-marker cube on notes tied to active quests (FO76-style) — user request from Nexus v1.1 comments. Visual side is easy (inject coloured symbol into entry text). Blocker is the detection side: FO4 quest objectives reference aliases rather than specific item FormIDs, so reliably mapping "this objective wants you to read that exact note" would be patchy. Half-working would be worse than nothing.
- [ ] Stats: "X of Y notes read" display somewhere in the Pip-Boy — wants a sensible injection point first; no obvious home for the text.

## Features — Future
- [ ] Custom icon for read/unread items (would need FallUI ExtraIcon integration or similar)
- [x] ~~Per-item "mark as unread" (via Pip-Boy interaction — maybe a hotkey while hovering?)~~ — DONE. Subsumed by the v1.1.0 toggle/mark feature: `bToggleKey` flips read state on the hovered item and `iMarkKey` flips mark state.
- [ ] Sorting options: alphabetical within read/unread groups, or by read order (most recently read first/last)
- [ ] **HUDMenu activation-prompt colour-tint for read items** — speculative dimming-surface idea. The global title-mod hook puts `(Read)` text everywhere the engine reads a name, but the activate-prompt colour stays default. Could intercept the prompt-text colour path (e.g. `fActivateRolloverTextColor` or equivalent) per-call based on whether the crosshair-targeted ref is a read item. Recon needed on both the colour path and a crosshair-ref → base-form lookup. Visual reinforcement, not a primary feature.
- [ ] **Dimming for vanilla Pip-Boy entries** — current alpha dim walks FallUI's renderer setup; vanilla Pip-Boy has a different SWF structure. If its entry rows have addressable display objects, we could replicate the alpha walk. Low priority — FallUI is the assumed setup for users who care about visual polish, and vanilla users still get the `(Read)` suffix as the primary signal.
- [x] ~~Support for non-FallUI setups (vanilla Pip-Boy UI)~~ — PARTIAL (discovered 2026-04-25). Suffix and toggle/mark keys work on the vanilla Pip-Boy because the entryList path resolves there too. Dimming doesn't — relies on FallUI's renderer setup. v1.3.0 description and readme reflect this; FallUI is now Recommended rather than Required.

## Technical Debt
- [x] ~~Review SEH exception handlers~~ — RESOLVED. All removed during cleanup. Hook code is stable. If crashes are reported, can add SEH wrapper around the hook body.
- [x] ~~Proper git history: squash/clean experimental commits before merging to develop~~ — DONE through v1.3.0. Develop's history is clean: every merged commit is a coherent semantic step (Conventional Commits throughout, no `wip:`/`fixup:` noise). The remaining `wip:` commits live on parked branches (`feature/perf-caching`, `feature/sort-to-bottom-wip`) which is the intended use of the prefix — paused drafts, not history.

## Publishing (v1.0 → v1.3.0 archaeology)

All items below are done as of v1.3.0; kept here as a checklist record.

- [x] ~~NexusMods page — full rewrite of description for v2 (pure C++ approach, no SWF patches)~~
- [x] ~~New screenshots showing dimming, "(Read)" suffix, config options~~
- [x] ~~Remove original mod file from NexusMods (old SWF-patching version)~~
- [x] ~~Review permissions — more permissive now (no FallUI SWFs, just our DLL)~~
- [x] ~~Thanks/credits section — FallUI/M8r (decompiled source reference), F4SE team~~
- [x] ~~Document F4SE dependency~~ — v1.3.0 covers OG (v0.6.23) and NG/AE (v0.7.x)
- [x] ~~Document FallUI Inventory dependency~~ — softened to Recommended in v1.3.0; works on vanilla too with reduced functionality
- [x] ~~No ESP/ESM required — DLL-only mod~~
- [x] ~~Release archive: just UnreadNotes.dll (INI auto-created on first run)~~
- [x] ~~Changelog / versioning~~ — CHANGELOG.md tracks 1.0 → 1.3.0
- [x] ~~Document INI settings with examples in the mod description~~
- [x] ~~Compatibility notes: GOG and Steam versions, other Pip-Boy mods~~

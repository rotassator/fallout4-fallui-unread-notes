# Changelog

All notable changes to UnreadNotes will be documented in this file.

## [1.3.0] — NG/AE runtime support

- Now works on NG (1.10.984) and AE (1.11.x) in addition to the existing
  OG (1.10.163) runtime. Single DLL covers all three — the correct runtime
  is detected at plugin load. Requires [Address Library for F4SE](https://www.nexusmods.com/fallout4/mods/47327)
  and an F4SE build matching the user's runtime.
- NG support is released without direct developer testing (no NG install
  available on the author's machine); AE is confirmed working end-to-end.
  If NG users hit issues, please report them on Nexus or the issue tracker.
- Cosaves are byte-compatible with 1.2.1 — upgrading keeps your read and
  marked sets intact. The `RdNt` / `MkNt` record format is unchanged.
- Ported from vanilla F4SE to CommonLibF4 + Address Library under the hood.
  Build system moved from CMake to XMake.
- The Pip-Boy display hook is now a vtable swap on PipboyMenu's
  AdvanceMovie slot, replacing the xbyak branch trampoline used in 1.2.1.
  Behaviourally identical from the user's perspective; internally, only
  PipboyMenu's AdvanceMovie is intercepted now, rather than every menu's
  call running through a trampoline and being filtered.
- New startup log line: `UnreadNotes: detected runtime X.Y.Z.B (OG|NG|AE)`
  prints near the top of `f4se.log` — makes bug reports self-diagnosing.

## [1.2.1] — Debug-flag scope fix

- `bMarkAllRead` now only marks notes and holotapes, not the entire MISC tab.
  Previously it would sweep in bobbleheads, quest items, and anything else
  sharing the misc filter bit — because the handler piggy-backed on the
  wider MarkableItems mask meant for manual keypress toggles.
- Version info is now embedded in the DLL: right-click `UnreadNotes.dll`,
  Properties, Details tab shows File version / Product version. Sourced
  from the CMake `project(VERSION ...)` so all on-disk and in-log
  versions stay in lockstep.

## [1.2.0] — Mark for later / excluded items

- Configurable second keypress (`iMarkKey` under `[Input]`) flags the selected
  item as "marked". Marked items stay bright, get a distinct suffix
  (`sMarkSuffix`, default `" (*)"`), and are skipped by auto-mark-as-read.
- Handles two user-requested cases with one mechanism:
  - Config holotapes (SKK Global Stash et al.) that you don't want dimmed
    just because you opened them.
  - Radiant / procedural notes that reuse FormIDs across contexts — marking
    one no longer marks them all as read.
- Toggle key and mark key are mutually exclusive: marking a read item clears
  read, toggling a marked item clears marked. An item is always in exactly
  one of three states: unread / read / marked.
- Cosave: new `MkNt` record persists the marked set alongside `RdNt`.
  Loader is back-compatible with 1.1 cosaves and bounds-checks record data.
- Warnings in the log if you configure the same key or suffix for both
  toggle and mark (misconfig would silently disable the mark branch or make
  states visually indistinguishable).

## [1.1.0] — Toggle key and broader coverage

- Configurable keypress to toggle read/unread on the selected Pip-Boy item.
  Set `iToggleKey` under the new `[Input]` section (commented out by default).
- Misc items (recipes, schematics, contracts like Shelley's) can now be
  marked via the toggle key. Auto-detection still skips them — they don't
  open BookMenu — but they'll dim and suffix like everything else.
- Magazines are auto-marked as read when opened through the Pip-Boy (they
  share the notes filterFlag and trigger BookMenu).
- Game cartridges are manually markable via the toggle key.
- Richer log messages for mark events: item type (FallUI tag or filterFlag
  label), name, FormID, and trigger source.

## [1.0.0] — Initial public release

First public release. Pure F4SE DLL plugin — no modified FallUI assets.

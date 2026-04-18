# Changelog

All notable changes to UnreadNotes will be documented in this file.

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

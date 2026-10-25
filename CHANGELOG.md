# UnHoarder changelog

## 1.2.19 - 2026-10-25

- Observe ground items (`type == 4`) directly during the native Automap/Corner Minimap unit render pass (`D2R+0xD76E0`) so minimap icons appear immediately on both the corner minimap and full-screen automap and stay visible while items remain on the ground without requiring `Alt` (Show Items) to be held.
- Check for custom `Filter01`–`Filter16` `.flac` audio assets at filter load time and fall back to the native drop sound with a warning if the audio files are not installed, preventing silent item drops on Non-Ladder installations without the audio pack.

## 1.2.18 - 2026-10-25

- Include quest weapons and jewelry (`quest > 0` in `weapons.txt`, `armor.txt`, and `misc.txt`, such as Wirt's Leg, Amulet of the Viper, and Horadric Malus) in `"ques"` and `"Quest"` `itemType` rules.
- Log a warning and safely ignore unknown or zero-item `itemType` selectors (such as `"Cloak"` or `"None"`) instead of rejecting the entire `filter.json`.

## 1.2.17 - 2026-10-25

- Fix minimap icons not appearing for controller ground labels (both standalone and alongside Controller QOL).
- Allow plugin initialization when a storefront update bumps the `D2R.exe` build version while `D2RLoader.exe` retains the verified native image; all native hooks and reads remain byte-verified.
- Compatible with Controller QOL 1.3.1+rev.93 and Inventory Deposit 1.3.1+rev.13.

## 1.2.16 - 2026-10-21

- Improve loot-label performance, including when Controller QOL is enabled.
- Skip inventory scanning when no active filter feature needs it.
- Hold either keyboard ALT key to pick up filter-hidden items with the mouse.
- Keep `/unhoarder perf` for performance reports; use QOL rev.89 when combining the plugins.

Live loot performance improved in user testing. Automated checks pass; full ALT,
controller and hosted interaction coverage remains open.

## 1.2.9 - background opacity compatibility

- Apply filter backgrounds regardless of the native black-label opacity.
  Preserve item, name, renderer and exact-black checks; malformed colors still
  retain native rendering.
- Use matching Controller QOL rev.80-beta.7-opacity.1 for the new artifact.
  Controller pickup and visibility behavior are unchanged.

Validation: 37 CTest tests, 51 Python source checks and 63 matching QOL tests
pass. The new regression fails against the old opacity guard. The user
confirmed the visual fix. No binary release is published by this source push.

## 1.2.8 - staged visibility update

- Share filter visibility with matching Controller QOL so hidden loot does not
  consume LB pickup slots. Use QOL rev.80-beta.7-visibility.2 for the latest
  unmodified-controller pickup handling as well.
- Retain the 1.2.7 label-only behavior: filtered labels can be hidden while
  native pickup remains available. UnHoarder no longer intercepts pickup actions.
- Preserve the controller labels, drop alerts and socket-rule fixes from 1.2.6.

Validation: 37 CTest tests, 51 Python source checks and 61 matching QOL tests
pass. Visible combined hide/pickup behavior, audible alert behavior and
exhaustive hosted-game combinations remain pending. No binary release has
been published for this branch. Original author credits and license are intact.

## 1.2.6 - staged fork update

- Apply filter names, colors and backgrounds to controller ground labels.
- Play filter alerts when items drop, even with labels hidden, with the normal
  drop sound retained when filter playback cannot be used.
- Make the 16 bundled filter alerts use the Items volume control. Requires the
  matching sound-table changes as well as the DLL.
- Restore socket-based show/hide rules with the reviewed Maps companion.

Includes the earlier 1.2.3-1.2.5 staged fixes. Existing filter rules and settings
are preserved. Original author: MindH1ve; this fork is maintained by Pyre-Flies.

Validation: 37 CTest tests, 49 Python source checks and matching QOL's 55 tests
passed. Controller labels were confirmed by the user; socket forwarding was
inspected read-only in game. Audible alert behavior and visible socket-rule
retesting remain pending. These changes have not been released as a binary here.

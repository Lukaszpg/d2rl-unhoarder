# Removing the GetItemCode detour (stacked PR)

## Immediate change

For D2R build 93847 UnHoarder formerly installed a pass-through inline
hook at `D2R+0x36EF50` merely to obtain a callable original trampoline.
No behavior or output was intercepted.

The first phase of this change **removes the detour** and instead:

- Verifies the same full 32-byte entry fingerprint before any call.
- Stores a native `GetItemCodeFn` pointing directly to the verified engine
  entry `D2RBase+0x36EF50`.
- Reuses this read-only function for native ground-label, pickup,
  automap, hover, drop-audio and visibility callbacks.
- Leaves the reader and all dependent behavior *inactive* when the entry
  is mismatched or belongs to another plugin. No foreign trampoline,
  guessed relocation or code patching is attempted.
- Does not change native item data, the SDK's public item handles or JSON.

This frees native hook ownership of `0x36EF50` for other plugins and
avoids installing a pass-through hook. It **does not** yet remove
the dependency on the game's qualified native helper. If another plugin
already detours this entry before UnHoarder starts, the reader will still
refuse to arm; it will not join or overwrite the owner's detour.

## Long-term SDK-only path (not enabled)

The SDK 0.4.0 `getItemDetails` API reads item-owned magic stat lists,
not item codes, and requires an opaque `ItemHandle`.
`getItemInfo` exposes `ItemInfo.code`, but only for handles obtained
through supported local-player inventory facilities, not arbitrary native
ground units.

The 0.4.0 `DataTableService` exposes read-only compiled
`DataTables::TableId::Items` rows after `DataTablesLoadedEvent`.
D2MOO 1.10 documents `D2ItemsTxt.dwCode` at offset `0x80`,
and `dwClassId` maps to an Items record index. This is only a historical
research lead, **not qualification for current D2R build 93847**.

Before replacing direct calls with classId-to-code lookup, validate
against live D2R compiled Items row stride, packed code field and at
least a cross-category reference set including new modded items;
synchronize refresh on data-table revision changes and refuse unknown
or partial snapshots. Game callback/table loading thread requirements
must be honored, with immutable read-only copies for UI and audio
callbacks. Never infer a row offset solely from D2MOO.

## Regression and acceptance

CI statically verifies removal of the detour and compilation/tests;
live testing must confirm ground-label Show/Hide, tooltip colors,
drop sounds, minimap icons, ALT+click pickups, game exit/rejoin,
foreign owners and the absence of any modification tracked at
`0x36EF50` with UnHoarder's plugin ID.

This PR is stacked on PR #11 and should be retargeted to `main` only
after the import is merged.

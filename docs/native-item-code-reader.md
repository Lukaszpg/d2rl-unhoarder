# SDK compiled classId-to-itemCode resolver (PR #12)

## Architecture

A native ground-item unit has a 32-bit class ID in its verified header
(`header[1]`). The code is resolved by a copied, immutable SDK index:

    classId -> PublishedItemCodes[classId] -> item code

UnHoarder performs **no hook, trampoline or native function call** to
D2R+0x36EF50. It does not enumerate external loose Excel files when
resolving runtime item codes. JSON item-type selectors retain their
independent existing Excel parsing.

## Qualification on game thread

1. The loader invokes our `DataTablesLoadedEvent` listener after its
   compiled tables are loaded, or `GameJoined` triggers a game-thread
   refresh from `DataTableService`.
2. `getTable(Rotw, Items)` provides the current row count, size,
   revision and read-only pointer; reject missing/unreasonable metadata.
3. `findRowByCode(Rotw, Items)` independently provides row pointers and
   indexes for multiple canonical items: cap, hax, rin, amu, r01.
4. Scan offsets within the compiled row and accept a **unique** four-byte
   code field matching at least four independent SDK probes. D2MOO's
   historical layout is not used as a current-build offset.
5. For **every** row, extract its code and independently require
   `findRowByCode` to return that same row index and revision.
   Any unknown, missing, duplicate or invalid row rejects publication.
6. Atomically publish the fully copied snapshot. Render, audio and
   pickup paths never retain or call through SDK/game table pointers.

Invalidate the snapshot before a new table revision is built; unknown
class IDs, failed qualification or missing codes fail open, preserving
native visibility/pickup/default audio.

### Runtime acceptance still required

SDK row-index identity must still be proven to equal the native
ground-item class ID in build 93847. A table built and validated against
SDK lookups proves compiled-table consistency, not that external native
units index the same records. Compare `ItemInfo.classId/code` for owned
items plus representative actual ground drops; include weapons,
armor, jewelry, runes, stackables and custom mod items. Verify
Show/Hide, pickup, ALT override, icon and audio on each type and after
Save & Exit. The log emits `LOOT_ITEM_CODES_READY` with rowSize,
codeOffset, count and revision on success or a specific REFUSED reason.

No live D2R acceptance has been performed solely by CI.

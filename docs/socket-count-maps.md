# Socket-count capability through the existing Maps hook

## Failure and supported scope

The user reported an unhighlighted rare Bastard Sword remaining visible.
Current rules first show socketed bases, then hide ordinary/non-ethereal,
zero-socket equipment through rare quality, then show unmatched items.
The socket rule is deliberately unresolved when the stat reader is missing;
the engine stops there and never guesses zero to reach a later Hide.

Startup logs report `LOOT_QUANTITY_UNAVAILABLE
reader-owner-no-compatible-service`. Socket count reused that general reader,
so this also disables socket rules. Read-only inspection of PID 42616 proved
the exact loader-1.3.1 bridge at game `0x2F5020` points through slot
`0x3E2A218` to the installed Maps stat hook, not directly to Core. Maps does
not publish the cooperative reader service expected by UnHoarder. This is
independent of controller painter admission and the unsupported hover path.

The repair adds a **socket-only** capability when the general reader is
unavailable. It does not admit arbitrary stats, change the general reader's
ownership restriction, assume unknown sockets are zero, bypass Maps, alter
Maps penalties, change rules or make new native patches.

## Exact reviewed native path

Maps artifact SHA256:
`D963CA4AAAFD70D2ED55864DCED956595290B523F8922C7D43BC0939FEB4E1A1`.
Core artifact SHA256:
`2A868D013D2E0830BD2D9E04B918B19E46A73CF726C833E70D089B948FDEB5A2`.
These are the currently loaded modules; unknown artifacts refuse this
capability. Their loaded images are pinned while disk identity is cached.

The Maps hook is module+`0x132F0`. Its 77-byte entry saves all three arguments,
calls the original reader once, saves the full RAX result at RSP+`0x30`,
then skips its stat-127 accounting branch for stat 194. The original-reader
slot at Maps+`0xFEB70` equals Core+`0x831DE0`. The 76-byte early-return gate
at Maps+`0x13373` handles null unit, nonzero layer and a null level callback,
or calls the stat-index helper. Its borrowed level callback slot is
Maps+`0xFEB78`; it is never invoked on the admitted socket path.

The stat-index helper at Maps+`0x142D0` subtracts 17 and compares the unsigned
result against 110. For stat 194, 177 exceeds that bound. The exact JA at
helper+`0x1C` goes to helper+`0x72` = Maps+`0x14342`, the 10-byte `return -1`
leaf. Neither switch lookup table is read. The gate receives that negative
index and returns the saved original value through Maps+`0x1359D` (8 bytes).
The player penalty/level adjustment branch is never reached for sockets.

Core+`0x831DE0` has the existing 16-byte wrapper and calls the existing
288-byte reader body at Core+`0x3D8BD0`. Both are checked in the loaded image.
The entire reachable Maps instruction path, forwarding/data slots, Core
wrapper/body, executable page ownership and read/write data provenance are
rechecked on both sides of each read. There is no cached live-code approval.
Unrelated Maps code does not gain admission for any other stat.

The capability invokes the **installed game bridge** with `(item,194,0)`.
It accepts only a type-4, nonzero/non-sentinel-ID item in proven ground or
dropping modes 3/5, preserves type/class/ID/mode across the call, and accepts
only a full 64-bit result in 0..15. The existing outer reader still makes
two matching scalar reads with its original item-identity checks. A failed
read never marks the scalar known. Pins are released on UnHoarder unload.
Pickup authorization remains strictly ground mode 3 in its separate guard.

## Validation

Read-only live audit passed all seven code-block checks, exact hashes,
bridge/forwarding slots and memory ownership/protection gates in PID 42616.
No native functions were invoked by that observation tool.

Portable tests mutate every relevant graph byte, reject missing/foreign
nodes and wrong continuations, and execute the captured Maps instructions
in a private process with a stub original. They prove one unchanged-argument
call and full return preservation for zero, valid counts, negative and
out-of-range values, with either null or non-null level-callback state.
The socket-only path never invokes that callback. A rare-sword rule replay
shows unknown sockets fail open, proven zero reaches Hide, and socketed or
ethereal cases retain their show behavior.

UnHoarder candidate is 1.2.6. Matching QOL remains based on rev.80-beta.7,
adding a separate exact 1.2.6 companion profile while preserving every older
profile and gameplay source. Maps itself is not rebuilt or replaced.
Final test totals, artifact hashes and installation status are recorded below
after validation. Visible hiding and live socket-read logging remain pending.

## Packaging requirement retained

The user explicitly requires the mod-supplied `sounds.txt` and its directory
structure in the extraction package. The Reimagined data payload must contain
`data/global/excel/sounds.txt`, extracted relative to `Reimagined.mpq`, using
the mod's complete current table with the 16 alert rows routed to Items.
Do not substitute a generic vanilla table or omit it from a DLL-only archive.
The launcher-clean source must retain the same routing (or a supported
post-restore data override must apply it) so preparation cannot undo it.
The private candidate includes a staged package-data file and manifest;
this is preparation, not a published release or a complete sound-asset kit.

Private fixtures, logs and native evidence remain under
`outputs/unhoarder-sockets-20261018/` and are not committed.

### Final validation and artifacts

- 37 UnHoarder CTest tests, 49 source checks, and 55 QOL CTest tests pass.
- UnHoarder 1.2.6 SHA256 `D6B71C25BDADA63F670E4B8FE50E50A000165FAD721E6C6F399D0177303083EB`.
- QOL beta.7-sockets.1 SHA256 `F3726603CB8821BE39735E38459F99651378DCEEFCE2FD364A57995F4DA65B0A`.
- Sole UnHoarder executable section RVA 0x1000, size 557199,
  SHA256 `8B25BB77651E50CF166CACB294EA2BB98D406CE53A90073127AD3471E6DC49D8`, no code relocations.
- New item wrapper/slot `0x1da70` / `0xb3488`;
  dispatch wrapper/slot `0x1e750` / `0xdb720`;
  Base `0xb3478`; filtering helper `0x25b20`;
  cache helper `0x1cc30`. Base independently recovered from two
  matching source-code windows: old 0x19B85 -> new 0x1A015, and old 0x1D345
  -> new 0x1DAF5. Each RIP operand resolves to the same Base; section shifts
  and global ordering were not used as address guesses.
- Automated source normalization proves every preexisting UnHoarder runtime
  function outside the socket reader is unchanged, apart from adding the
  isolated capability include and unload cleanup. No native hooks were added.
- Exact old/current/new actual-DLL admission fixtures pass. Changing a loaded
  filtering helper still refuses QOL admission despite a known disk hash.
- Maps binary, general-stat admission, filter JSON and sound tables unchanged.

### Installation

Installed both candidates in the root and Reimagined loader plugin directories
on 2026-10-18 at 11:39 EDT, after confirming the game and target modules were
unloaded. All four deployed DLL hashes match the tested candidates. Rollback
copies and the installation receipt are retained in the private staging folder.
The installer verified all 138 other plugin/config files unchanged. Both active
and launcher-clean sound tables retain SHA256
`B9303D841EBC26B22BFB2471C199659D880EAC5D456B01C6859DAC35C005A8A1`.
The new DLLs have not yet been loaded in game; visible rare-sword hiding remains
pending a restart and gameplay check.


## 1.2.16: fresh paired witness reads

`Memory` has an optional pair reader with narrow-read fallback. Each guard can
combine Head/Gate, the original/level pointer slots, and Index/Negative into one
fresh bounded snapshot per pair. The production reader only combines ranges on
one hardware page within 512 bytes, using the unchanged section/kind/current
protection/allocation-owner checks and fault-contained copy. Optional gaps or
unusual sections/pages fall back to both original narrow reads. Nothing is
retained between pairs, guards, property calls or frames.

Every compared byte, RIP-relative operand, pointer alignment and continuation
predicate remains. The four before/after guards and two equal socket values are
unchanged. A private-process before/after run measured four guards at 59.834 vs
44.654 microseconds (7 samples of 2,000 iterations), about 25% lower. This is
routine cost, not a predicted FPS gain. Tests cover fresh mutation observations,
code/data kind, allocation owner, guard/no-access/read-only pages, overflow,
wide/cross-page fallback, and the full graph's byte mutations/native execution.

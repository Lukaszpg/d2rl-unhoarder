# UnHoarder native drop-sound replacement and Items volume

## Requested behavior and previous limitation

The user confirmed controller labels work with the 1.2.4 repair, then found
that sounds wait for visible labels. They requested actual-drop timing,
replacement of the ordinary drop sound when a rule selects a filter sound,
ordinary-sound fallback otherwise, and the Item volume slider.

Versions through 1.2.4 feed audio from the mouse label formatter and optional
in-world label compatibility observer. This is observation of rendering,
not a drop event; the controller formatter added in 1.2.4 intentionally did
not feed sound. Queued named playback was additional to native sound.
The bundled Filter01..Filter16 rows also used ambient channels and IsUI=1.

## Build-specific discovery

Evidence is the privately captured loaded-game `.text`, `.rdata` and `.pdata`
for the supported 93787/93847 route, checked against the current loader's
existing exact PE identity and native named-sound backend guards. The new
route has not yet been observed executing in the game; installation/startup
and audible timing/slider confirmation must be recorded separately.

- `D2R+0x1CFB278` is the `dropsound` compiler field name. Its references at
  `0x316AD2`, `0x31D220`, `0x31F4E2` establish the common item, SetItems and
  UniqueItems compiled fields. The common item loader is `0x315FD0..0x31868B`
  and loads weapons/armor/misc; set loader `0x31C2D0..0x31E303`; unique loader
  `0x31F1D0..0x31FFB3`.
- Compiled dropsound is a signed 16-bit field: common item row+`0x134`, set
  row+`0x86`, unique row+`0x88`. The adjacent byte parameters used by the
  native player are common+`0x138` and set/unique+`0x94`. Their full meaning
  is not required or overridden by this repair.
- `0x1C3420..0x1C34E8` is a drop-sound-ID selector used by numerous inventory
  and interaction paths; **not** suitable as an actual-drop event.
- `0x1C8130..0x1C8270` is the native item drop-audio routine, one argument in
  RCX (item pointer), void return. It selects the common/set/unique drop
  sound and calls native player `0x1A0780` at `0x1C8226` and `0x1C823F`.
  The only captured direct callers are CALL `0x1C369B`, return `0x1C36A0`,
  and CALL `0x1C6C99`, return `0x1C6C9E`.
- Mode transition routine `0x1C35E0..0x1C37EF` writes the new mode to unit+12
  at `0x1C3632`. Its seven-entry jump table `0x1C37D4..0x1C37EF` maps mode 5
  to `0x1C3659`; that branch invokes the drop-audio routine at `0x1C369B`.
  The table maps mode 3 to a different branch, so the player is not a
  per-frame standing-ground observation.
- Packet/item-arrival routine `0x1C6B60..0x1C6CBF` creates/resolves a type-4
  unit, decodes its data, then checks mode 3 at `0x1C6C66` and item flag
  `0x2000` at `0x1C6C80`. The admitted branch sets mode 5 through
  `0x1C6C91` and calls drop audio at `0x1C6C99` with the same RSI unit.
- Named player `0x1A0C00..0x1A0C57` resolves the row name through `0xD9760`
  and forwards its RDX item, R8 and R9 unchanged into the same native audio
  player `0x1A0780`. It returns the native playback handle, or null when
  name resolution/playback is rejected. A handle is not audibility proof.

The complete 321-byte native drop routine, both 71-/72-byte caller branches,
28-byte mode jump table and 88-byte name-player function are guarded by
`src/native_drop_sound.hpp` (580 bytes total). Unknown changes refuse this
audio extension. Installation displaces exactly the first 20 bytes: four
five-byte nonvolatile-register saves, with no RIP-relative operands or
branches. SDK-managed trampoline and teardown retain native forwarding.

## Replacement contract

The hook admits only the two proven caller returns, nonzero/non-sentinel
type-4 item identities and modes 3/5. It reads type/class/ID/mode before rule
resolution and rechecks them before playback. A missing/unreadable/stale
identity, unsupported caller, missing rule sound, unavailable backend or null
playback handle forwards the original native drop routine exactly once.

A matching rule invokes the qualified name player synchronously with the
same native item and arguments `(rowName,item,0,0)`. Only a non-null accepted
playback handle suppresses the ordinary drop routine. No label callbacks
feed sound, no item pointer outlives the native callback, no server drop/TC
state is changed and no authoritative-server-thread queue is needed. This
also avoids the SDK runOnGameThread limitation on remote clients, while
remaining inside the proven original client audio call context.

Each native drop event is eligible independently, including an immediate
pickup/re-drop of the same ID. There is no render-frame replay, visibility
trigger or 1.5-second label-baseline suppression. Existing item property
readers, rules, pickup guards and controller label guards remain unchanged.

## Item volume routing

Native amulet/ring/rune/jewel rows use `sfx/items_sd`, redirecting to HD item
rows on `sfx/items_hd`. All 16 bundled filter rows are changed to
`Channel=sfx/items_hd`, `IsUI=0`, matching the Item audio category rather
than ambient/UI routing. Their files, gain ranges, other flags, order and
all other rows remain byte-for-byte unchanged. The active `sounds.txt` has
no accompanying sounds.bin in the mod directory. Restart is required for
the native table to load the changed routing.

This sets the stock Filter01..Filter16 alerts' bus; arbitrary third-party
sound row names still use their own sounds.txt channel. It does not globally
reroute native monsters/ambient effects when their sound names are reused
in a rule. The volume slider's audible effect needs live confirmation.

## Validation and artifacts

New portable tests exercise replacement success, missing selection, rejected
playback, repeated drops, both callers/modes, non-drop/label callers, changed
identities, read failures and mutation of every guarded byte. Source checks
ensure neither label observer can trigger audio and no audio queue retains
item pointers. Existing artifact, rules, native property and rendering suites
continue to run. Matching QOL retains the beta.7 gameplay source and all three
previous exact companion profiles, adding a distinct profile for this build.

Private receipts, binaries, sound-table before/after copies and test logs stay
under `outputs/unhoarder-drop-sound-20261018/`. Final hashes, installation and
test totals are added after validation. Native execution, audible timing,
fallback and the Item slider remain pending live verification.

### Final candidate validation

- UnHoarder 1.2.5 SHA256 `99F490678D700BEBBB03FEBA09CF67C88AE4BA94C64A6181E53849FB66EC207A`.
- QOL beta.7-drops.1 SHA256 `365365ECC7AEA4045C391DF33B81D94EFE2D20F10711FF54684725E9BADB4D0B`.
- 35 UnHoarder CTest tests, 48 source checks and 52 QOL tests pass, including
  old/previous/new actual-DLL admissions and modified-live-code refusal.
- Beta.7 gameplay source preserved; only the enumerated metadata,
  compatibility and test files differ from the last labels.1 candidate.
- Exact companion profile and text hash are in private `drop-profile.json`.
  Item wrapper `0x1D2C0`, slot `0xB0428`; dispatch wrapper `0x1DFA0`, slot
  `0xD86C0`; Base `0xB0418`; filtering helper `0x250B0`, cache helper `0x1C710`.
  Sole executable section RVA `0x1000`, size 550943, no text relocations,
  SHA256 `CF696B63AA909BF0C0B417000A4C4DC8023A7ED5CC1C162BE698E904579F63CC`.
- Sound-table transformation changes precisely Channel and IsUI on the 16
  bundled filter rows; every other byte/field is preserved. No gain, asset,
  filter rule, item definition or normal sound row was changed.
- Audible behavior and slider response remain unverified until restart.

### Installation

Installed at 10:39:08 EDT with the game and target modules unloaded. Four DLL
hashes and the active sound table match the tested candidates. All 138 other
plugin/config files are unchanged; filter.json was not written. A rollback
copy and restore map for all five replaced files are retained under the
private output folder, rollback-beta7labels1-unhoarder124-20261018-103904.
Audible behavior and the Item slider still require an in-game retest.

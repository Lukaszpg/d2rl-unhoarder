# UnHoarder background opacity refusal (2026-10-18)

UnHoarder 1.2.2 artifact SHA-256:
`7AB7F748C9C110B3EC91E5C69D9A21125EBD7BC1E23DC82E475C563DD992FDCF`.
Local D2R build 3.3.93847; executable identity is recorded in the Controller
QOL UnHoarder coexistence records. This is a separate plugin-side paint guard
issue, not a JSON parse failure or evidence that QOL must be loaded.

The shared sets/uniques rule requests background `RGBA(110,35,160,0.82)`.
Logs confirm the rule loads and background handling arms. Its amulet also
matches Filter02 sound selection and draws the configured minimap diamond.
Adding explicit white text was user-confirmed visible after hot reload.

Local UnHoarder `src/plugin.cpp:313` defines native baseline
`OriginalGroundBackground={0,0,0,0.6f}`. Its shared-paint middleware around
line 3680 requires a byte-exact match to that baseline before forwarding the
configured background, even after item identity and rule selection succeed.

A read-only native observation in game PID 47884 found unit 137, visible name
`Amulet (99)`, with background `{0,0,0,0.8f}`. Exact observed bits:
`00000000 00000000 00000000 3F4CCCCD`; expected alpha is `3F19999A`.
This observed background cannot pass the production comparison. Do not broaden
the guard or remove it without qualifying the legitimate native baseline and
preserving the caller, item, text, epoch and ownership checks.

## Captured native route and probe limits

- Prior private mapped-code capture: game `0x14FE122` calls `0x15178F0`, then
  `0x14FE12A` passes its return in RCX to label painter `0x1517960`.
- Getter `0x1517917`: `48 8D 05 52 7D 4D 01` returns static label-vector
  storage at game `0x29EF670`. The same return is at `0x1517950` after init.
- Painter consumes a record pointer at vector `+0`, count at `+8`, stride
  `0x144`; native unit ID is record `+0x10`, RGBA `+0x14`, text argument
  `+0x24`, and printable narrow text `+0x28`.
- At `0x1517AE6` R8 points to record `+0x14`; `0x1517AED` RDX points to
  record `+0x24`; `0x1517AF1` calls shared painter `0x1FA8E0` and returns
  to the existing qualified return site `0x1517AF6`.
- Probe guards the exact getter bytes, bounds count to 4096, reads records
  without calling game functions, and rechecks the vector header afterward.
  It observed 32 slots, many with invalid unit IDs. The third header field is
  not proven to be a capacity; an initial capacity interpretation was rejected.

Private probe and snapshot:
`outputs/unhoarder-amulet-diagnosis-20261018/read_label.py` and
`label-records.json`. Captured game images are not committed.

No UnHoarder DLL/background guard was changed during this diagnosis. The sound
submission path returned successfully, but initial audibility was not proven
by that return. The user later confirmed sounds after removing an older QOL
installation; the exact cause of that sound change was not established.

The user subsequently requested white text for both sets and uniques in their
shared rule and removal of the temporary set-amulet override. That config-only
edit preserves the purple background, Filter02 sound, minimap icon and all
other original rules. It does not repair the opacity comparison.

## Proven source of 80% and implemented repair

The installed Reimagined UI profile
`data/global/ui/layouts/_profilehd.json`, `TooltipStyle`, explicitly supplies
`inGameBackgroundColor: [0,0,0,0.8]`; its
`inGameShowItemsSelectedBackgroundColor` is `[0,0.25,0.5,1]`.
The controller `_profilelv.json` contains the same two values. The profile's
comments identify these as the ordinary in-game label and selected Show Items
label backgrounds. Thus 80% is mod UI data, corroborated by the native record,
while 60% was UnHoarder's historical hardcoded admission assumption. No UI
profile or game transparency setting needs to be changed to repair this.

The new `ground_label_background.hpp` production predicate recognizes only
exact black RGB with alpha bits `3F19999A` (previously admitted 60%) or
`3F4CCCCD` (profile-backed, runtime-observed 80%). Negative/positive adjacent
float representations, other opacities, nonblack RGB, NaN/infinity, selected
blue and already-configured purple are refused. It remains behind the same
caller, paired pointers, copied native color, recent item identity, rule,
name-match, enabled-state and hide predicates. A source normalization audit
confirms no other runtime plugin code changed. Rendering still forwards a
temporary argument; native label/item memory is never recolored persistently.

Regression: the new test first failed with the captured 80% record under the
old predicate, then passed with the two exact native cases. All 33 UnHoarder
CTest tests (including ABI/resource/exports/role), 47 source tests and 46 QOL
CTest tests passed. QOL uses the verified `rev.80-beta.7` source baseline,
SHA-256 `8DC16CCF626A6E0CBDBF2899550E57EFD8C09C7C469D8E6D21C30D8185715D45`.
All caption, cube/input/gameplay files are unchanged. Only companion admission,
test registration and candidate version metadata differ among existing files.

## Rebuilt artifact and native coexistence ledger

UnHoarder local candidate 1.2.3:
`E7606D9ED5B79DA80A50F1EF9AD8A94D9AF84259247D08D3D177B1CFE8F405F6`.
QOL beta.7-based candidate `1.3.1+rev.80-beta.7-bg.1`:
`AB84EA6C20D50BD2FE2538588FFE3DE1E1B4E0C7F5A645C5428C77EA736C2587`.

The new companion is independently qualified. The original 1.2.2 profiles
remain supported unchanged; neither version strings nor a known disk hash can
override an altered loaded graph. The new artifact has one executable section
at RVA `0x1000`, 549,263 bytes, no base relocations within it; its loaded-code
SHA-256 must be
`BE6128DFB379FC3333B70CCE2206523693DA40C766BCA319EC406ADE4885F7C8`.

| New UnHoarder RVA | Complete static contract |
| --- | --- |
| `0x1CC60..0x1CC72` | Exact 19-byte item-code wrapper: reserve 40 bytes, load original slot, call once with unchanged RCX, restore stack, return unchanged RAX. |
| `0xB0428` | Item-code original slot, unchanged from 1.2.2. SDK trampoline must replay five native bytes and continue at game `0x36EF55`; original game tail remains exact. |
| `0x1D880..0x1D8E0` | Exact 97-byte dispatcher wrapper. Same complete instruction/ABI structure as 1.2.2, with independently checked RIP-relative operands. Movement actions 2/4 forward all four arguments once. Action 22/type 4 alone enters the pickup filtering helper. |
| `0x24F40` | Dispatcher pickup-decision helper, preserved source behavior and covered by complete live executable fingerprint. |
| `0x1C0B0` | Allowed pickup price-cache invalidation helper, covered by complete live executable fingerprint. |
| `0xD86B8`, `0xB0418` | Dispatcher original slot and native Base, unchanged; writable non-executable companion image ownership is mandatory. Trampoline must replay exact prologue and resume at game `0xFABE5`; all remaining native dispatcher bytes stay exact. |

Native fixture tests execute both exact old/new wrappers and test each graph
byte, unreadable node and memory kind. Additional tests load the actual old
and new DLLs into an isolated test process, build synthetic native relays and
trampolines, and exercise the production admission code. Mutating a reachable
loaded helper outside the wrapper is refused despite matching disk identity;
restoring it restores admission. These fixtures never open the game process.

Installed both matching DLLs in the existing root and Reimagined plugin
locations on 2026-10-18 at 09:44 EDT, after checking game/server process and
module state. Hash inventory preserved all 138 other plugin/config files,
including the white sets/uniques rule. Rollback and private receipts are under
`outputs/unhoarder-bg-fix-20261018/`. No release was published. Visible purple
background and post-install live coexistence still require a fresh-game check.

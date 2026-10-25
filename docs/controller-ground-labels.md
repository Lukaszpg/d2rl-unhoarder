# UnHoarder controller ground-label route

## Problem and exact-build evidence

On 2026-10-18 the user confirmed that mouse labels receive filter styling,
while all controller ground labels retain their default appearance. The
running game was `D2RLoader.exe` PID 44152, image base `0x140000000`, supported
93787/93847 route. It loaded UnHoarder 1.2.3 and the previously validated
Controller QOL `rev.80-beta.7-bg.1` candidate from Reimagined's plugin directory.

The controller producer calls the shared formatter `D2R+0x1FA9F0` at
`0x151A2AD`, returning to `0x151A2B2`. Its arguments differ from the two
previously admitted mouse producer calls:

- RCX is the exact item unit in R15. Lookup at `0x151A22B` requests type 4;
  the mode query at `0x151A274` must return 3 before formatting.
- RDX is controller record RSI + `0x24`, with the printable name at +`0x28`.
- R8 points to temporary stack geometry at RSP+`0x30`, **not** the record.
- R9 and the two stack arguments retain the existing six-argument formatter
  ABI. The wrapper forwards them unchanged once.
- Geometry is copied to the record at `0x151A31C`. The native caller writes
  the actual item ID to record+`0x10` at `0x151A324`, **after** formatting.
  Testing that field during the inner name writer therefore rejects a
  legitimate controller record on its initial construction.

Controller painting uses `0x1519BB0` with record stride `0x148`. Its item
lookup at `0x1519C36` uses record+`0x10` and type 4. At `0x1519E36`, R8 is
record+`0x14`; RDX at `0x1519E3D` is record+`0x24`. It calls the shared painter
`0x1FA8E0` at `0x1519E41`, returning to `0x1519E46`. The old middleware
admitted only mouse return `0x1517AF6` despite logging both return sites.

The controller vector getter `0x1519B40` returns static vector header
`0x29F26F0` through the LEA at `0x1519B67`. A read-only snapshot found 15
records, including ground item IDs and the same text/+`0x14` RGBA layout.
The mouse vector remains at `0x29EF670`, stride `0x144`. Count is header+8;
the third header field is not qualified as capacity. Invalid IDs remain
`0xFFFFFFFF`. Neither vector is modified by the observation script.

## Guarded repair

The controller route is independently enabled only if all 168 bytes in four
complete witnesses match: formatter setup/call `0x151A263..0x151A2B1`, native
record publication `0x151A307..0x151A326`, item lookup/layout
`0x1519C23..0x1519C43`, and painter argument setup/call
`0x1519E2E..0x1519E45`. All four matched the running process read-only.
The complete byte arrays live in the affected source's
`src/controller_ground_label_witnesses.hpp`; unknown changes refuse the
controller extension while preserving the existing mouse route.

Only the admitted formatter caller establishes a nested, thread-local scope
for an exact unit pointer, output pointer and nonzero/non-sentinel item ID.
The inner writer can use that identity before the native caller publishes
the record ID. It still requires its existing exact inner return, 0x80-byte
output ABI, readable item/header/name, current rules and bounded name write.
The scope restores its predecessor after forwarding and observation. Mouse
calls still require their original record/ID pairing.

The observer reads the controller name through that exact scoped output;
it never treats R8 geometry as a record. The controller painter retains the
existing record/argument pairing, recent item identity, exact visible name,
rule resolution and native black-background checks. Temporary paint argument
and glyph color scopes remain temporary. No item, geometry, record-ID or
background memory is persistently written. Native blue selection remains
unchanged. Sound and native-hover integration are outside this repair.

## Candidate and coexistence

UnHoarder 1.2.4 local candidate SHA256:
`130E04F1CE7AF27724077682DE2DC980679ACE2ECCFF62514F4BEE93A6DB616C`.
Its sole executable section is RVA `0x1000`, size 550351, no text relocations,
SHA256 `0753CCCFA1487104AA570373A9C04F4FC273A9EF55956EB4B0D1A36CF852FAC3`.
Item-code forwarding wrapper moves to `0x1CE40`, dispatch wrapper to
`0x1DC20`; original slots remain `0xB0428` and `0xD86B8`, Base `0xB0418`.
Dispatch filtering helper is `0x252E0`, price-cache helper `0x1C290`.

Matching QOL candidate derives from the already validated beta.7 source,
version `1.3.1+rev.80-beta.7-labels.1`. It adds a separate exact artifact,
wrapper/slot/trampoline/continuation and whole-loaded-text profile, retaining
both earlier admitted UnHoarder profiles. It does not change cube, input,
caption, pickup routing or gameplay behavior. Installation and final tests
are recorded separately; a successful build is not visible game validation.

## Validation status

- Read-only live: all four native controller witnesses match; controller
  vector stride, IDs, text and 80% black baseline observed.
- UnHoarder: 34 CTest tests and 47 source checks pass, including artifact
  admission. New scope tests reject unrelated pointers, IDs and inactive
  scopes; mutation of every witness byte refuses the route.
- Combined QOL candidate: 49 CTest tests pass, including both earlier actual
  DLL admissions, new actual DLL admission, all graph-byte mutations, native
  wrapper forwarding and mutated loaded-helper refusal. All gameplay sources
  match the validated beta.7 background-fix baseline; only metadata and the
  enumerated compatibility/test files differ.
- Visible controller behavior: pending restart and user verification.

Installed at 10:11:47 EDT with the game and target modules unloaded. Both
loader plugin directories now contain UnHoarder 1.2.4 and QOL
`rev.80-beta.7-labels.1` (SHA256
`5F9B5F6D611C7EAA84FC951104E40CDC486ADE7BC53508E782DBA1751A9B4096`).
All four deployed hashes match their candidates; 138 other plugin/config
files are unchanged. Configurations were not written. Recoverable rollback
copies and the exact restore map are under the private output directory's
`rollback-beta7bg1-unhoarder123-20261018-101143/` subdirectory.

Private captured bytes, logs, build outputs and read-only snapshots stay
under `outputs/unhoarder-controller-labels-20261018/` and are not committed.

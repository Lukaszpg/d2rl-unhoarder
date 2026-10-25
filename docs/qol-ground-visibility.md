# Unhoarder visibility and Controller QOL shortcuts

## Observed mismatch

With Unhoarder 1.2.7 and QOL rev.80-beta.7-labels.1, the user saw three LB
shortcut assignments for three ground items although Unhoarder hid one label.
Live QOL logs from PID 29856 showed three GUIDs assigned and native pickup
requests submitted for those IDs. This is separate from native pickup denial:
native pickup must remain available for filtered items.

QOL tracks label observations in its own GUID/time cache. Unhoarder conceals
the later bulk painter output without removing the native record. Therefore
QOL's formatter observations cannot establish Unhoarder visibility.

## Cooperative ABI

Unhoarder 1.2.8 publishes `unhoarder-ground-visibility`, version 1, through the
SDK PluginCommunicationService. The provider ID is `unhoarder`. Its 16-byte
standard-layout table contains uint32 size/version then an eight-byte cdecl
noexcept callback pointer. Callback arguments are a synchronous borrowed
native unit pointer and uint32 GUID/native item code; the uint32 result is
Unknown=0, Visible=1, Hidden=2. No pointer is retained and no item state is
written. This service is presentation advice, never native pickup authority.

The provider checks item type 4, requested ID and ground mode 3; it reads the
native code through its qualified original item-code reader and canonicalizes
trailing NUL/space padding. It resolves the current ordered rule chain using
existing guarded property readers, then checks the stable four-word header,
published rules snapshot and active presentation mode again. Uncertain reads,
unresolved conditions and generation changes return Unknown. Inactive filter
presentation returns Visible. No native hook or lookup address was added.

QOL queries through an SDK broker lease scoped to each call, including release
after the callback. It verifies provider, service name/version, minimum table
size, internal size/version and nonnull callback. Late provider loading works
without a persistent raw function pointer or module-unload race.

Only a positive Hidden result excludes a candidate. No provider, older
Unhoarder, an unavailable/malformed service or Unknown preserves QOL's existing
filter and native eligibility behavior. Existing QOL filtering remains in
effect when the provider is available as well.

## Assignment and submission

The existing native candidate read now applies the visibility query before
returning a candidate. This covers initial LB assignment and the fresh read
already required by Submit before creating a native pickup permit. The
existing queued approach route also calls Submit and gets the same recheck.

Held assignments keep their frozen GUID/button mapping. A fresh, explicitly
hidden decision clears only the affected slot; it cannot substitute a new
item under a held button. The next LB press assigns compact visible slots.
Missing/unknown visibility does not clear held assignments. Range, ordering,
approach, existing pickup filters, native forwarding and continuation guards
are unchanged.

The additional held-slot check uses the already proven native unit lookup
`game+0x9A5D0` with `(guid,4)` solely to obtain a synchronous item pointer;
the provider revalidates identity. Failure is ignored, preserving the normal
held snapshot. No new native address or state write is introduced.

## Exact binary admission

The new Unhoarder item-code wrapper RVA is `0x1DF60`, original slot `0xB2488`;
19 bytes: `48 83 EC 28 48 8B 05 1D 45 09 00 FF D0 90 48 83 C4 28 C3`.
Artifact SHA256:
`9854DFBD082F1FC103CA30ED355E07A8E5F5741206F370D732E26E939005FAE7`.
QOL adds an exact identity/profile alongside the old admitted profiles. Native
entry, relay, image ownership, wrapper, slot, trampoline and continuation
checks are retained. Unhoarder still installs no pickup dispatcher hook.

## Validation

Private candidates/evidence: `outputs/unhoarder-visibility-20261018`.
Tests cover broker lease lifetime, Hidden/Visible/Unknown decisions, missing
provider, malformed service version, initial compact assignments, frozen held
assignments, clearing a hidden slot without substitution, and preservation
when priorities/distances change. Source checks cover fresh identity/rule
generation checks and lack of native pickup interception. Actual DLL admission
and mutation refusal are tested in a synthetic process, not the running game.
Final automated/deployment/live states are recorded after completion.

Automated validation: 37 Unhoarder tests, 51 source checks and 60 QOL
tests pass. QOL artifact SHA256 `FC4F950B67F5DAF193D6D98B75C35BBAAC9BC3FD4826B853976B57710A4E68C4`.
Source normalization confirms no Unhoarder change outside this isolated
service and its publication. Deployment and visible validation pending.

## Installation

Installed both candidates in root and Reimagined loader directories at
12:18 EDT on 2026-10-18 after confirming game/module closure. All four DLL
hashes match the tested artifacts; rollback copies and installation receipt
are retained in private staging. All 138 other plugin/config files were
verified unchanged. No audio table or configuration was written. Live
visibility/slot behavior remains pending a new game process and user retest.

## Unmodified controller pickup follow-up

The user confirmed the LB shortcut integration works, then found unmodified
controller A still picks up a hidden item. The active QOL configuration has
`block_filtered_pickup = true`. QOL's client interaction filter and local
native pickup wrapper previously checked only `PlacardOverlay::IsPlacardActive`;
an Unhoarder-concealed bulk label still has a QOL formatter observation.

The QOL-only visibility.2 candidate incorporates the same read-only service
decision into both existing filters. `HiddenForPickup(guid)` locks the loot
state, requires Ready/context, revalidates the complete native profile, reads
the live ground-item code and uses the existing guarded visibility query.
Only explicit Hidden adds a block; uncertain/service-unavailable reads retain
the original QOL behavior. It adds no hooks, addresses or native writes.

The native/server pickup wrapper must not call this UI reader. UI formatter
observations and the client interaction check refresh an owned GUID/code,
local-session-generation and monotonic-timestamp proof. The native wrapper
reads only that copied proof via `CachedHiddenForPickup`, after the existing
local-actor guard. Proofs expire after 250 ms, reject future timestamps and
old sessions, are cleared with the existing loot caches on lifecycle changes,
and are erased on a failed/visible UI query or a removed QOL observation.
The bounded 2048-entry cache stores no native pointer. An absent, expired or
unresolved proof adds no block. The client interaction path still queries
fresh visibility before request submission.

The client guard still requires plugin enabled, block-filtered-pickup enabled
and active controller UI, with the original gold exemption. The native
wrapper still forwards remote actors and valid one-use completion permits,
keeps controller/settings scope and gold exemption, and retains modifier
handling. Keyboard/mouse pickup is not newly blocked. Unhoarder is unchanged
and remains presentation-only when used without QOL.

This follow-up changes neither the leased service ABI nor the admitted
Unhoarder 1.2.8 artifact/profile. Private evidence is retained in
`outputs/qol-native-visibility-20261018`; automated/deployment state follows
completion. User confirmation applies to the earlier LB fix, not yet to this
new unmodified-A guard.

Visibility.2 automated validation: 61 QOL tests pass, including actual
companion admission/mutation refusal, service fallback/lifetime tests,
both pickup-path wiring checks and unmodified-A policy cases.
QOL SHA256 `2BEDF657EE1ACF64A379450F5438826D8A6201D4A8A650D249CF4B361BB0B851`. Unhoarder 1.2.8 is unchanged.
Deployment and visible A-button validation remain pending.

Visibility.2 installed in both loader directories at 12:28 EDT on 2026-10-18
after verifying game/module closure. Both deployed QOL hashes match the
tested candidate; rollback copies and receipt are retained in private staging.
All 140 other plugin/config files, including Unhoarder and Maps, were verified
unchanged. No configuration or sound table was written. Fresh-game unmodified
A-button validation remains pending.

## User confirmation and local package

The user subsequently confirmed that visibility.2 fixed controller pickup.
Keyboard/mouse pickup of hidden items remains intentionally available; QOL's
controller-only block applies when `block_filtered_pickup` is enabled.

Packaged the exact deployed Unhoarder 1.2.8 and QOL visibility.2 artifacts as
`outputs/packages/reimagined-loot-20261018/Reimagined-Unhoarder-1.2.8-Controller-QOL-rev.80-beta.7-visibility.2.zip`.
Extract relative to `mods/Reimagined`. Both complete mod-supplied tables use
the existing `Reimagined.mpq/data/global/excel/sounds.txt` and
`Reimagined.mpq/data/global/excel_launcher_clean/sounds.txt` paths. Both retain
the previously recorded Items-routing hash. Existing Reimagined sound assets
are prerequisites and are not duplicated in this focused upgrade.

At the user's request, no TOMLs, skill config or active filter JSON are shipped.
Only `reference/filter.json` is included, containing the requested comment
directing users to the Reimagined loot-filter website. It is not a loadable
filter and is outside all live configuration paths.

Archive SHA256:
`B9AECCF9E340B1542CAACEBAAC5EC3116629F34D1EA6EA4139B89BDC12890469`.
ZIP integrity, exact payload/deployed hashes, internal and external checksums,
x64 exports/ABI manifest/metadata/imports, release QOL artifact verification,
16 Items-routed rows, privacy scan and extraction over customized config/other
plugin sentinels pass. No installed files were modified. No publication was
performed; private packaging receipt is adjacent to the archive.

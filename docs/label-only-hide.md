# Historical label-only hide policy (1.2.7 through 1.2.15)

Superseded for keyboard/mouse pickup in 1.2.16 by the user-requested
[ALT pickup policy](keyboard-alt-pickup.md). Presentation behavior remains.

## Cause and behavior

User testing of 1.2.6 found that hidden equipment could not be picked up.
The socket-count repair exposed an existing, intentional pickup restriction:
`HookNativeActionDispatch` rejected action 22 / item type 4 when the current
rule resolved to hide. This is a feature policy error, not a socket-reader
or Maps failure. A hide rule must affect normal loot-label presentation only.

Unhoarder 1.2.7 does not call `InstallNativePickupGuard` during initialization.
Consequently it does not hook the native dispatcher at game RVA `0xFABE0`.
Native pickup and other plugins' independently configured pickup behavior are
preserved. The previous guard implementation remains unregistered; its old
ABI profiles remain available to QOL for older plugin versions.

The qualified hover-render callback now returns false for a hidden rule,
allowing the native targeted label to draw. The regular bulk-ground painter
continues to conceal qualified hidden labels. Ground identity, caller, row,
name, rendering, socket and ownership checks are unchanged. No native item
state writes or extra native hooks were added.

## Exact companion profile

QOL remains based on rev.80-beta.7. Its only runtime change is admission of
the newly built Unhoarder item-code forwarding wrapper. The new 19-byte
wrapper is at Unhoarder RVA `0x1DA70`; its original-function slot is
`0xB1488`. Exact bytes:
`48 83 EC 28 48 8B 05 0D 3A 09 00 FF D0 90 48 83 C4 28 C3`.
The wrapper calls the original once with unchanged arguments and return.
Admission still requires exact artifact identity, executable image ownership,
the complete native entry/relay/wrapper/original slot/trampoline graph, and
the native continuation. A changed wrapper is refused despite a known disk
hash. There is no new dispatch-predecessor profile because this version does
not install a dispatch hook; QOL uses its existing native admission path.

Unhoarder SHA256:
`9F4A219D134B31FF996ACEE6D7AAD04B1067060F9C1204206157BDB65D883DA8`.

Private build/test/installation evidence is retained under
`outputs/unhoarder-label-only-20261018`. Automated validation and deployment
status will be recorded after completion. Visible pickup remains pending
fresh gameplay validation; source and fixture tests do not prove it.

Automated validation: 37 Unhoarder tests, 50 source checks, 57 QOL tests pass.
QOL artifact SHA256 `96712047F284F03101D06897BED1D6777E66755F94902C7CEE9C657D140AEA44`. Source normalization proves
only the intended hover behavior and pickup-hook registration changed in
Unhoarder; all QOL gameplay code is unchanged.

## Installation

Installed Unhoarder 1.2.7 and QOL rev.80-beta.7-labels.1 in both root and
Reimagined loader directories on 2026-10-18 at 12:05 EDT. The game and target
modules were unloaded before replacement. All four deployed hashes match
the tested artifacts. Rollback copies and `installation.json` are retained
in the private staging directory. All 138 other plugin/config files were
verified unchanged; no audio tables or configurations were written.
Fresh-game visible label/pickup validation is still pending.

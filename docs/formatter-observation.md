# Formatter-local presentation observation (1.2.14)

## Problem and boundary

The rev.86 / 1.2.13 mixed-loot test still reached about 43 FPS. The latest
complete live interval reported about 2,974 property evaluations, 1,812 socket
observations and 4,786 quality observations per second. Timing scopes overlap;
never sum them or translate these callback counts directly into frame time.

The existing exact native path enters UnHoarder `HookLabelFormatter` at game
RVA `0x1FA9F0`. Its original formatter calls the native name writer at
`0x1FAA18`; QOL's existing post-name bridge at `0x1FAA1D` then runs `Track`.
For potentially hidden items this invokes QOL's unchanged `HiddenForPickup`,
including its complete native admission and leased visibility query. After the
original formatter returns, UnHoarder calls `RememberGroundIdentity` and used
to evaluate the same filter properties again for presentation.

This is an observed source/guarded-call-graph composition point, not a general
hook chain. Existing producer caller returns remain `0x15171EA` and
`0x1517788`; the independently qualified controller return is `0x151A2B2`.
No new game RVA, native ABI, detour or SDK service is introduced. These remain
exact-build contracts (93847 native fingerprints; the existing 93787 coarse
build-name gate does not substitute for them).

## Ownership and safety

A stack-owned `GroundFormatterObservation` is active only while the existing
formatter hook runs. An eligible native producer/output pairing is required.
The observation flows ONE WAY: a newly executed visibility query may supply
owned scalar values to the same call's later presentation identity. A visibility
or pickup query never reads the observation, including a second query in the
same call. QOL's native admission, native unit/code lookup, service lease,
250 ms session-scoped hidden proof and action-time checks are unchanged.

The existing complete native property readers are unchanged: two quality/data
observations, requested-flag comparison, current page checks, two socket values,
all Maps forwarding/ownership guards and fail-open rule ordering remain.
Presentation uses the completed observation from earlier in this one formatter
operation, just as the existing painter uses owned formatter observations. It
does not claim a second observation of mutable properties at the later paint
identity step. Nothing is reused by a subsequent pickup query or another frame.

Publication requires the original visibility query to pass all its end guards
and match rules. Additional 24-byte item snapshots before/after the query agree
on type, class, id, strict ground mode 3 and item-data pointer. Code, rule snapshot,
hook/arming state and formatter epoch are rechecked. Consumption is single-use
and checks the exact borrowed unit/output pairing, current 24-byte header,
code, current owning rule snapshot, epoch and arming again. The scope retains
its rule snapshot to prevent address-reuse ambiguity. Only copied scalar values
enter the existing pointer-free paint identity. Quantity/vendor-dependent
filters retain the independent presentation reading.

Any nested formatter invalidates its parent's observation. Any later visibility
query invalidates the earlier observation even if that query fails before doing
property work. Existing identity reset sites also advance a formatter epoch.
The thread-local pointer restores the parent stack scope on exit; no borrowed
native pointer or page/native-admission proof survives that operation. Failed
handoff checks fall back to the original guarded presentation evaluation.

## Independent plugins

There is no new runtime dependency, service table, registration or coordinated
load order. Without QOL, UnHoarder evaluates presentation normally. Without
UnHoarder, QOL keeps its existing native behavior. Missing services, rejected
QOL compatibility, older companions or unqualified formatter paths retain the
existing fallback. Both read-only perf commands remain available independently.
Matching rev.87 admits the new exact UnHoarder DLL; this is ordinary artifact
compatibility, not a dependency for standalone use.

## Validation and interpretation

The actual formatter -> visibility -> identity fixture went red on the old
source: 1,800 labels caused 3,600 property and socket observations. The new path
uses 1,800 each. The combined Research fixture compiles actual QOL `Track`,
`HiddenForPickup`, SDK lease helpers and provider callbacks. It retains all
1,800 QOL admission/native-code calls while reducing property and socket
observations from 3,600 to 1,800 and quality observations from 7,200 to 3,600.
Native QOL entry implementations and socket execution are owned spies, not game
calls. Thus these are work counts, not an FPS forecast.

Old/new source produce the same result digest for 3,840 combinations of Continue,
quality, sockets, ethereal state, missing properties and ground/presentation
modes. Tests cover independent operation, missing provider, failed admission,
rule reload, identity/data-pointer/mode/code changes, arming, reset epochs,
nested calls, early query failure, thread isolation and fresh subsequent pickup.
Existing byte/page/forwarding mutation tests run separately against real mapped
candidate artifacts in an owned process. Live mixed-loot acceptance is pending.

`unhoarder perf` adds `formatter-observation-published`,
`formatter-observation-reused`, and `formatter-observation-fallback`. A reuse
count near the full visibility-query count would confirm that this overlap is
active in the game. Name-only work and native renderer cost remain separate.

# Label work: UnHoarder 1.2.13 / Controller QOL rev.86

The previous pair improved the user's gem-label test, but the mixed-loot
interval ending October 21, 2026, 20:09:42 EDT remains expensive. Only 25% of
labels prove no Hide is possible. QOL processes about 809 labels/s at 0.255
ms/call, including 606 full visibility calls/s. UnHoarder observes 2,202
property calls/s (309 elapsed ms/s), including 1,388 socket observations/s
(141 elapsed ms/s). Inventory polling is 0.831 elapsed ms/s. These are nested
elapsed scopes, not additive CPU totals, frame times or distinct-item counts.
The active quality/ethereal/socket equipment rules still need the full query.

## Changes and preserved contracts

- Resolve the actual minimap icon before sampling position. The native name
  callback skips four position reads when there is no icon. The generic
  compatibility observer retains identity observation; its style callback
  samples position after resolving an icon. Both still clear a previous icon
  on a no-icon outcome. Icon-bearing items retain both complete unit/path
  observations, page qualification, identity and coordinate checks. Quantity,
  name, palette, sound and rule-order behavior are unchanged.
- Quality/item-level observation retains both 24-byte unit snapshots and their
  complete comparison, plus both 64-byte item-data observations. Each data copy
  follows a fresh full-span `GroundCandidatePageReadable` check and uses the
  existing SEH-contained local copy. The second page qualification is new;
  no successful page proof is reused. Quality at +0, level at +0x38 and
  requested flags at +0x18 retain all range/equality checks. The data pointer
  remains +0x10. These are existing build-93847 fields, not new discoveries.
  Unreadable or changing values remain unknown/fail open.
- The Maps socket guard reads ordinary PE metadata with one fresh 4 KiB
  `ReadProcessMemory` snapshot per image instead of three API copies. Headers
  outside that snapshot, or failure to read its optional tail, use the original
  narrow reads. All DOS/NT/machine/size/section/overlap checks remain. No metadata
  survives the invocation. Every live code/data/trampoline read still qualifies
  current pages, section, ownership and bytes. QOL applies the same change only
  to item-code compatibility; its other reader users keep the existing mode.

`MapsSocket::Guard`/`Read`, `ReadNativeGroundSockets`, full visibility service,
no-Hide preflight, property acquisition order and QOL pickup/action checks are
unchanged. There are still two socket observations, each with full live admission
before and after the native call. No successful admission or mutable property
snapshot is cached across callbacks. The filter is unchanged. Potentially hidden
equipment still takes the full decision path.

## Exact paired artifact

UnHoarder 1.2.13 SHA256:
`9412c5f6175918b5b11ba7de631d4f795ee87cde37cab9b9b5fd35328aac32f8`.
Image size `0xE9000`; forwarding wrapper RVA `0x1F530`; original slot `0xB6A18`.
Release link-map symbols and DLL bytes establish the unchanged one-argument,
unchanged-return forwarding wrapper:

```text
48 83 EC 28 48 8B 05 DD 74 09 00 FF D0 90 48 83 C4 28 C3
```

QOL adds an exact profile while retaining all eleven older profiles. Full
game-entry/tail, relay, wrapper, slot, trampoline, continuation and current
page/ownership checks remain. No new native game RVA or hook is added.

## Evidence and limits

The production-name-hook fixture fails against the saved old callback with
7,200 position reads for 1,800 ordinary labels; the candidate performs zero.
Icon, mode-5, mutation, reload, generic compatibility and quantity cases retain
their results. The property fixture retains 3,456 ordered-rule parity cases and
tests identity/quality/flag changes, page refusal, and protection changes after
either page qualification. The common-layout PE test fails before with three
API reads and passes with one; header mutation, large layouts and inaccessible
optional tails are covered. Actual-DLL admission tests mutate every wrapper
and trampoline byte, slot, continuation, current protections and DOS metadata
after warm admission.

An owned-memory benchmark (9 samples of 5,000 calls per routine) measured
quality-reader median 2.373 to 1.925 microseconds and PE-header median 1.384 to
1.014 microseconds, including equal quality timing instrumentation. These are
about 19% and 27% savings in those isolated routines on this host, not overall
plugin, game or FPS estimates. No game native function executes in the benchmark.
Mixed-loot FPS and visible behavior with this new pair remain untested.

`unhoarder perf` adds `native-quality-observation`, `minimap-position`, and
`minimap-position-no-icon`; the last is a work counter with zero elapsed time.
Existing counters and `qol perf` remain. Retain the same mixed-loot scene,
filter and settings; measure 30 seconds with labels shown and 30 seconds hidden,
recording both reports and observed FPS. Do not add nested timings or infer
FPS from minimap-render callback frequency.
